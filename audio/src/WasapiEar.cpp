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

#include "WasapiCommon.h"

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

constexpr int SCAN_MS = 1000;
constexpr int PUMP_MS = 20;
// 100 ms, in WASAPI's 100 ns units: long enough that a late pump loses nothing.
constexpr REFERENCE_TIME BUFFER = 1000000;
// Ids for the pretend graph's streams and clients, clear of the inputs'.
constexpr uint32_t STREAM_IDS = 0x80000000u;

using namespace wasapi;

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
