// EarAudio on Windows, through WASAPI in shared mode (EarAudio.h).
//
// One thread of its own, in COM's multithreaded apartment, does it all:
// once a second it lists the active inputs and the audio sessions on each
// (a session's process id says whose it is), finds data2g-host's among
// them as the PipeWire Ear finds it in PipeWire's graph, and picks the
// input to record (ear::pickSource); in between it drains the capture
// every 20 ms. Shared mode at the card's own format, so data2g-host, which
// records in shared mode too (miniaudio's default), shares the card and
// never notices.
#include "ragtime/audio/EarAudio.h"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ragtime
{
namespace audio
{

namespace
{

// The interfaces' ids, written out so neither __uuidof nor libuuid is needed.
const GUID MM_DEVICE_ENUMERATOR = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const GUID I_MM_DEVICE_ENUMERATOR = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID I_AUDIO_CLIENT = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const GUID I_AUDIO_CAPTURE_CLIENT = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
const GUID I_AUDIO_SESSION_MANAGER2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
const GUID I_AUDIO_SESSION_CONTROL2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
// PKEY_Device_FriendlyName: "USB Audio CODEC" and the like.
const PROPERTYKEY FRIENDLY_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr int SCAN_MS = 1000;
constexpr int PUMP_MS = 20;
// 100 ms, in WASAPI's 100 ns units: long enough that a late pump loses nothing.
constexpr REFERENCE_TIME BUFFER = 1000000;
// Ids for the pretend graph's streams and clients, clear of the inputs'.
constexpr uint32_t STREAM_IDS = 0x80000000u;

// A COM pointer, released when it goes.
template <typename T>
class Com
{
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put()
    {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* release()
    {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void reset()
    {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

std::string utf8(const wchar_t* text)
{
    if (!text || !*text) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out((std::size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text)
{
    if (text.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out((std::size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
    return out;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A process's program, as PipeWire would name it: "data2g-host" for
// C:\...\data2g-host.exe.
std::string programOf(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = utf8(path);
    CloseHandle(process);
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.erase(name.size() - 4);
    return name;
}

// The device's own id, "{0.0.1.00000000}.{...}": the source's name.
std::string deviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return "";
    std::string out = utf8(id);
    CoTaskMemFree(id);
    return out;
}

std::string friendlyName(IMMDevice* device)
{
    Com<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put()))) return "";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(props->GetValue(FRIENDLY_NAME, &value)) && value.vt == VT_LPWSTR) out = utf8(value.pwszVal);
    PropVariantClear(&value);
    return out;
}

// How the card's samples are laid out, from its mix format.
struct Layout
{
    bool ok = false;
    bool isFloat = false;
    int bits = 0;           // the container's: 16, 24 or 32
    int channels = 0;
    int stride = 0;         // bytes a frame
    int rate = 0;
};

Layout layoutOf(const WAVEFORMATEX* w)
{
    Layout l;
    if (!w || w->nChannels == 0) return l;
    l.channels = w->nChannels;
    l.rate = (int)w->nSamplesPerSec;
    l.stride = w->nBlockAlign;
    l.bits = w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        l.isFloat = true;
    else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22)
    {
        // KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT differ only in Data1.
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        if (x->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT) l.isFloat = true;
        else if (x->SubFormat.Data1 != WAVE_FORMAT_PCM) return l;
    }
    else if (w->wFormatTag != WAVE_FORMAT_PCM)
        return l;
    l.ok = (l.isFloat ? l.bits == 32 : (l.bits == 16 || l.bits == 24 || l.bits == 32)) &&
           l.stride >= l.channels * l.bits / 8 && l.rate > 0;
    return l;
}

float sampleAt(const BYTE* p, const Layout& l)
{
    if (l.isFloat)
    {
        float f;
        std::memcpy(&f, p, sizeof f);
        return f;
    }
    if (l.bits == 16)
    {
        int16_t v;
        std::memcpy(&v, p, sizeof v);
        return (float)v / 32768.0f;
    }
    if (l.bits == 24)
    {
        const int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
        return (float)((double)v / 2147483648.0);
    }
    int32_t v;
    std::memcpy(&v, p, sizeof v);
    return (float)((double)v / 2147483648.0);
}

class WasapiEar : public EarAudio
{
public:
    ~WasapiEar() override { stop(); }

    void start(Samples samples, Changed changed) override
    {
        stop();
        samples_ = std::move(samples);
        changed_ = std::move(changed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = false;
            poked_ = true;
        }
        thread_ = std::thread([this] { run(); });
    }

    void stop() override
    {
        if (thread_.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                quit_ = true;
            }
            wake_.notify_all();
            thread_.join();
        }
        samples_ = nullptr;
        changed_ = nullptr;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = EarStatus();
    }

    void choose(const std::string& sourceName) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chosen_ = sourceName;
            poked_ = true;
        }
        wake_.notify_all();
    }

    void setHostPid(std::optional<int> pid) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pid == hostPid_) return;
            hostPid_ = pid;
            poked_ = true;
        }
        wake_.notify_all();
    }

    EarStatus status() const override
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        EarStatus s = status_;
        const int64_t last = lastSamplesMs_.load();
        s.streaming = s.listening != 0 && last != 0 && nowMs() - last < 1000;
        return s;
    }

private:
    void run()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Com<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(MM_DEVICE_ENUMERATOR, nullptr, CLSCTX_ALL, I_MM_DEVICE_ENUMERATOR,
                                    devices.putVoid())))
        {
            setProblem("Windows' audio service isn't answering");
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return quit_; });
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.available = true;
                status_.problem.clear();
            }
            loop(devices.get());
        }
        closeCapture();
        devices.reset();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    void loop(IMMDeviceEnumerator* devices)
    {
        int64_t lastScan = 0;
        for (;;)
        {
            bool poked = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (quit_) return;
                poked = poked_;
                poked_ = false;
            }
            if (poked || lastScan == 0 || nowMs() - lastScan >= SCAN_MS)
            {
                scan(devices);
                evaluate(devices);
                lastScan = nowMs();
            }
            if (capture_) pump();
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(PUMP_MS), [this] { return quit_ || poked_; });
        }
    }

    // The inputs, and data2g's sessions on them, as a graph PipeWire's way:
    // an input is a source, a data2g session a capture linked from it.
    void scan(IMMDeviceEnumerator* devices)
    {
        ear::Graph graph;
        uint32_t next = STREAM_IDS;
        const DWORD self = GetCurrentProcessId();
        Com<IMMDeviceCollection> list;
        UINT count = 0;
        if (SUCCEEDED(devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, list.put())))
            list->GetCount(&count);
        for (UINT i = 0; i < count; i++)
        {
            Com<IMMDevice> device;
            if (FAILED(list->Item(i, device.put()))) continue;
            ear::GraphNode input;
            input.name = deviceId(device.get());
            if (input.name.empty()) continue;
            input.id = idFor(input.name);
            input.description = friendlyName(device.get());
            input.mediaClass = "Audio/Source";
            graph.nodes.push_back(input);

            Com<IAudioSessionManager2> manager;
            if (FAILED(device->Activate(I_AUDIO_SESSION_MANAGER2, CLSCTX_ALL, nullptr, manager.putVoid()))) continue;
            Com<IAudioSessionEnumerator> sessions;
            int n = 0;
            if (FAILED(manager->GetSessionEnumerator(sessions.put())) || FAILED(sessions->GetCount(&n))) continue;
            for (int j = 0; j < n; j++)
            {
                Com<IAudioSessionControl> session;
                Com<IAudioSessionControl2> session2;
                if (FAILED(sessions->GetSession(j, session.put()))) continue;
                if (FAILED(session->QueryInterface(I_AUDIO_SESSION_CONTROL2, session2.putVoid()))) continue;
                AudioSessionState state = AudioSessionStateInactive;
                DWORD pid = 0;
                // S_OK only: a session shared by several processes names none.
                if (FAILED(session2->GetState(&state)) || state != AudioSessionStateActive) continue;
                if (session2->GetProcessId(&pid) != S_OK || pid == 0 || pid == self) continue;
                const std::string program = programOf(pid);
                if (!ear::isData2GProgram(program)) continue;
                ear::GraphClient client;
                client.id = next++;
                client.appName = program;
                client.pid = (int)pid;
                ear::GraphNode stream;
                stream.id = next++;
                stream.clientId = client.id;
                stream.name = program;
                stream.mediaClass = "Stream/Input/Audio";
                stream.appName = program;
                graph.clients.push_back(client);
                graph.nodes.push_back(stream);
                graph.links.push_back({input.id, stream.id});
            }
        }
        graph_ = std::move(graph);

        defaultSource_.clear();
        Com<IMMDevice> fallback;
        if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, fallback.put())))
            defaultSource_ = deviceId(fallback.get());
    }

    // A source's id, the same for as long as the Ear is open.
    uint32_t idFor(const std::string& name)
    {
        auto it = ids_.find(name);
        if (it != ids_.end()) return it->second;
        const uint32_t id = (uint32_t)ids_.size() + 1;
        ids_[name] = id;
        return id;
    }

    void evaluate(IMMDeviceEnumerator* devices)
    {
        std::string chosen;
        std::optional<int> hostPid;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chosen = chosen_;
            hostPid = hostPid_;
        }
        const ear::HostSearch host = ear::findHostCapture(graph_, hostPid);
        if (host.found && host.found->source != 0) lastHostSource_ = host.found->sourceName;
        const ear::SourcePick pick = ear::pickSource(graph_, chosen, host, lastHostSource_, defaultSource_);
        const ear::GraphNode* want = pick.node;

        std::string problem;
        if (!want)
            closeCapture();
        else if (want->id != target_)
        {
            closeCapture();
            problem = openCapture(devices, *want);
        }
        else
            problem = captureProblem_;

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.sources = ear::sources(graph_);
            status_.host = host;
            status_.following = chosen.empty();
            status_.chosen = chosen;
            status_.pick = target_ != 0 ? pick.why : ear::Pick::None;
            status_.listening = target_;
            status_.listeningName = target_ != 0 ? want->name : "";
            status_.listeningDescription =
                target_ != 0 ? (want->description.empty() ? want->name : want->description) : "";
            status_.rate = target_ != 0 ? layout_.rate : 0;
            status_.problem = problem;
        }
        notify();
    }

    // "" when it opened, or why not.
    std::string openCapture(IMMDeviceEnumerator* devices, const ear::GraphNode& source)
    {
        Com<IMMDevice> device;
        if (FAILED(devices->GetDevice(wide(source.name).c_str(), device.put()))) return "That input isn't there";
        Com<IAudioClient> client;
        if (FAILED(device->Activate(I_AUDIO_CLIENT, CLSCTX_ALL, nullptr, client.putVoid())))
            return "Windows wouldn't open that input";
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) return "That input didn't say its format";
        const Layout layout = layoutOf(mix);
        HRESULT hr = E_FAIL;
        if (layout.ok) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, BUFFER, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (!layout.ok) return "That input's format isn't one the Ear reads";
        // Opened elsewhere in exclusive mode, a shared stream can't join.
        if (hr == AUDCLNT_E_DEVICE_IN_USE) return "Another program has that input to itself (exclusive mode)";
        if (FAILED(hr)) return "Windows wouldn't record from that input";
        Com<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(I_AUDIO_CAPTURE_CLIENT, capture.putVoid())) || FAILED(client->Start()))
            return "Windows wouldn't record from that input";
        client_ = client.release();
        capture_ = capture.release();
        layout_ = layout;
        target_ = source.id;
        captureProblem_.clear();
        lastSamplesMs_ = 0;
        return "";
    }

    void closeCapture()
    {
        if (client_) client_->Stop();
        if (capture_) capture_->Release();
        if (client_) client_->Release();
        capture_ = nullptr;
        client_ = nullptr;
        target_ = 0;
        layout_ = Layout();
        lastSamplesMs_ = 0;
    }

    void pump()
    {
        UINT32 packet = 0;
        HRESULT hr = S_OK;
        while (SUCCEEDED(hr = capture_->GetNextPacketSize(&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            mono_.assign(frames, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
            {
                const float share = 1.0f / (float)layout_.channels;
                const int bytes = layout_.bits / 8;
                for (UINT32 f = 0; f < frames; f++)
                {
                    const BYTE* frame = data + (std::size_t)f * (std::size_t)layout_.stride;
                    float sum = 0;
                    for (int c = 0; c < layout_.channels; c++) sum += sampleAt(frame + c * bytes, layout_);
                    mono_[f] = sum * share;
                }
            }
            capture_->ReleaseBuffer(frames);
            if (frames > 0 && samples_) samples_(mono_.data(), mono_.size(), layout_.rate);
            lastSamplesMs_ = nowMs();
        }
        if (FAILED(hr))
        {
            // Unplugged, or the format changed under it: look again.
            closeCapture();
            captureProblem_ = hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The input went away" : "Recording stopped";
            std::lock_guard<std::mutex> lock(mutex_);
            poked_ = true;
        }
    }

    void setProblem(const std::string& problem)
    {
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.available = false;
            status_.problem = problem;
        }
        notify();
    }

    void notify()
    {
        if (changed_) changed_();
    }

    Samples samples_;
    Changed changed_;
    std::thread thread_;

    // Asked of the Ear's thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    bool poked_ = false;
    std::string chosen_;
    std::optional<int> hostPid_;

    // The Ear's thread's own.
    ear::Graph graph_;
    std::map<std::string, uint32_t> ids_;
    std::string lastHostSource_;
    std::string defaultSource_;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    Layout layout_;
    uint32_t target_ = 0;
    std::string captureProblem_;
    std::vector<float> mono_;
    std::atomic<int64_t> lastSamplesMs_{0};

    mutable std::mutex statusMutex_;
    EarStatus status_;
};

} // namespace

std::unique_ptr<EarAudio> makeEarAudio()
{
    return std::make_unique<WasapiEar>();
}

} // namespace audio
} // namespace ragtime
