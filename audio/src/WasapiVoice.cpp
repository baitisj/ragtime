// VoiceAudio on Windows, through WASAPI in shared mode (VoiceAudio.h).
//
// One thread of its own, in COM's multithreaded apartment, as the Ear's:
// once a second it lists the active inputs and outputs, with data2g's
// audio sessions on each (a session's process id says whose it is), as a
// graph PipeWire's way: an input is a source, an output a sink, a data2g
// session on an input a capture linked from it, one on an output a
// playback linked to it, and each endpoint's container (the physical
// device) its card. voice::devices then leaves the radio's out. In
// between it drains the microphone and feeds the speaker every 10 ms.
//
// The microphone records at the card's mix format, mixed down to mono, as
// the Ear does. The speaker plays the clip as it is, mono at its own
// rate, with the audio engine converting (AUTOCONVERTPCM).
#include "ragtime/audio/VoiceAudio.h"

#include "WasapiCommon.h"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ragtime
{
namespace audio
{

namespace
{

using namespace wasapi;

constexpr int SCAN_MS = 1000;
constexpr int PUMP_MS = 10;
// 100 ms, in WASAPI's 100 ns units.
constexpr REFERENCE_TIME BUFFER = 1000000;
constexpr uint32_t STREAM_IDS = 0x80000000u;
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

class WasapiVoice : public VoiceAudio
{
public:
    ~WasapiVoice() override { stop(); }

    void start(Changed changed, const std::vector<std::string>& remembered) override
    {
        stop();
        changed_ = std::move(changed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remembered_ = voice::rememberRadio({}, remembered);
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
        Finished finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished = std::exchange(request_.finished, nullptr);
            request_ = Clip();
            listening_ = false;
            samples_ = nullptr;
        }
        if (finished) finished(false);
        changed_ = nullptr;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = VoiceStatus();
    }

    void chooseMicrophone(const std::string& name) override
    {
        poke([&] { chosenMicrophone_ = name; });
    }

    void chooseSpeaker(const std::string& name) override
    {
        poke([&] { chosenSpeaker_ = name; });
    }

    void forgetRadio() override
    {
        poke([&] { remembered_.clear(); });
    }

    void listen(Samples samples) override
    {
        poke([&] {
            samples_ = std::move(samples);
            listening_ = true;
            listenGeneration_++;
        });
    }

    void stopListening() override
    {
        poke([&] {
            listening_ = false;
            samples_ = nullptr;
        });
    }

    void play(std::vector<float> samples, int rate, Finished finished) override
    {
        begin(std::move(samples), rate, false, std::move(finished));
    }

    void playStream(int rate, Finished finished) override { begin({}, rate, true, std::move(finished)); }

    void append(const float* samples, std::size_t n) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!streamOpen_ || !samples) return;
        // Into the clip not started yet, or beside the one playing.
        std::vector<float>& to = request_.pending ? request_.samples : appended_;
        to.insert(to.end(), samples, samples + n);
    }

    void endStream() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!streamOpen_) return;
        streamOpen_ = false;
        if (request_.pending)
            request_.open = false;
        else
            ended_ = true;
    }

    void stopPlaying() override
    {
        Finished before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            before = std::exchange(request_.finished, nullptr);
            request_ = Clip();
            request_.generation = ++playGeneration_;
            request_.pending = true;
            streamOpen_ = false;
            appended_.clear();
            ended_ = false;
            poked_ = true;
        }
        wake_.notify_all();
        if (before) before(false);
    }

    double playPosition() const override
    {
        const int rate = playingRate_.load();
        return rate > 0 ? (double)handed_.load() / rate : 0.0;
    }

    void setVolume(float gain) override { volume_ = std::max(0.0f, gain); }

    VoiceStatus status() const override
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        VoiceStatus s = status_;
        const int64_t last = lastCaptureMs_.load();
        s.recording = s.listening && last != 0 && nowMs() - last < 1000;
        return s;
    }

private:
    struct Clip
    {
        std::vector<float> samples;
        int rate = 0;
        Finished finished;
        uint64_t generation = 0;
        bool open = false;      // more is coming (playStream)
        bool pending = false;   // asked for, not yet taken by the thread
    };

    void begin(std::vector<float> samples, int rate, bool open, Finished finished)
    {
        Finished before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A clip asked for but not started yet is replaced here.
            before = std::exchange(request_.finished, nullptr);
            request_.samples = std::move(samples);
            request_.rate = rate;
            request_.finished = std::move(finished);
            request_.generation = ++playGeneration_;
            request_.open = open;
            request_.pending = true;
            streamOpen_ = open;
            appended_.clear();
            ended_ = false;
            poked_ = true;
        }
        wake_.notify_all();
        if (before) before(false);
    }

    template <typename Fn>
    void poke(Fn fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn();
            poked_ = true;
        }
        wake_.notify_all();
    }

    void run()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Com<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(MM_DEVICE_ENUMERATOR, nullptr, CLSCTX_ALL, I_MM_DEVICE_ENUMERATOR,
                                    devices.putVoid())))
        {
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.available = false;
                status_.problem = "Windows' audio service isn't answering";
            }
            notify();
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
        Finished finished = closePlayback();
        if (finished) finished(false);
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
                std::lock_guard<std::mutex> lock(mutex_);
                if (quit_) return;
                poked = poked_;
                poked_ = false;
            }
            if (poked || lastScan == 0 || nowMs() - lastScan >= SCAN_MS)
            {
                if (poked || lastScan == 0 || nowMs() - lastScan >= SCAN_MS)
                {
                    scan(devices);
                    lastScan = nowMs();
                }
                evaluate(devices);
            }
            if (capture_) pumpCapture();
            if (render_) pumpPlayback();
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(PUMP_MS), [this] { return quit_ || poked_; });
        }
    }

    // The endpoints, data2g's sessions on them and their containers.
    void scan(IMMDeviceEnumerator* devices)
    {
        ear::Graph graph;
        uint32_t next = STREAM_IDS;
        const DWORD self = GetCurrentProcessId();
        for (const EDataFlow flow : {eCapture, eRender})
        {
            const bool input = flow == eCapture;
            Com<IMMDeviceCollection> list;
            UINT count = 0;
            if (SUCCEEDED(devices->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, list.put()))) list->GetCount(&count);
            for (UINT i = 0; i < count; i++)
            {
                Com<IMMDevice> device;
                if (FAILED(list->Item(i, device.put()))) continue;
                ear::GraphNode endpoint;
                endpoint.name = deviceId(device.get());
                if (endpoint.name.empty()) continue;
                endpoint.id = idFor(endpoint.name);
                endpoint.description = friendlyName(device.get());
                endpoint.mediaClass = input ? "Audio/Source" : "Audio/Sink";
                endpoint.device = containerId(device.get());
                graph.nodes.push_back(endpoint);

                Com<IAudioSessionManager2> manager;
                if (FAILED(device->Activate(I_AUDIO_SESSION_MANAGER2, CLSCTX_ALL, nullptr, manager.putVoid())))
                    continue;
                Com<IAudioSessionEnumerator> sessions;
                int n = 0;
                if (FAILED(manager->GetSessionEnumerator(sessions.put())) || FAILED(sessions->GetCount(&n))) continue;
                for (int j = 0; j < n; j++)
                {
                    Com<IAudioSessionControl> session;
                    Com<IAudioSessionControl2> session2;
                    if (FAILED(sessions->GetSession(j, session.put()))) continue;
                    if (FAILED(session->QueryInterface(I_AUDIO_SESSION_CONTROL2, session2.putVoid()))) continue;
                    // Any state: data2g-host's output is the radio's even
                    // between bursts, while its session sits inactive.
                    DWORD pid = 0;
                    AudioSessionState state = AudioSessionStateExpired;
                    if (FAILED(session2->GetState(&state)) || state == AudioSessionStateExpired) continue;
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
                    stream.mediaClass = input ? "Stream/Input/Audio" : "Stream/Output/Audio";
                    stream.appName = program;
                    graph.clients.push_back(client);
                    graph.nodes.push_back(stream);
                    if (input)
                        graph.links.push_back({endpoint.id, stream.id});
                    else
                        graph.links.push_back({stream.id, endpoint.id});
                }
            }
        }
        graph_ = std::move(graph);

        defaultSource_.clear();
        defaultSink_.clear();
        Com<IMMDevice> fallback;
        if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eCommunications, fallback.put())))
            defaultSource_ = deviceId(fallback.get());
        if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, fallback.put())))
            defaultSink_ = deviceId(fallback.get());
    }

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
        std::string chosenMicrophone, chosenSpeaker;
        bool listening = false;
        uint64_t listenGeneration = 0;
        Clip request;
        bool newClip = false;
        std::vector<std::string> remembered;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chosenMicrophone = chosenMicrophone_;
            chosenSpeaker = chosenSpeaker_;
            listening = listening_;
            listenGeneration = listenGeneration_;
            remembered = remembered_;
            if (request_.pending)
            {
                request = std::move(request_);
                request_ = Clip();
                request_.generation = request.generation;
                newClip = true;
            }
        }
        const voice::Devices found = voice::devices(graph_, remembered);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remembered_ = voice::rememberRadio(remembered_, found.radioNow);
        }
        const voice::DevicePick microphone =
            voice::pickDevice(found.microphones, found, chosenMicrophone, defaultSource_);
        const voice::DevicePick speaker = voice::pickDevice(found.speakers, found, chosenSpeaker, defaultSink_);

        // The microphone.
        std::string microphoneProblem;
        if (!listening)
            closeCapture();
        else if (!microphone.usable())
        {
            closeCapture();
            microphoneProblem = voice::describe(microphone, true);
        }
        else if (microphone.node.name != captureName_ || listenGeneration != captureGeneration_)
        {
            closeCapture();
            microphoneProblem = openCapture(devices, microphone.node.name);
            captureGeneration_ = listenGeneration;
        }
        else
            microphoneProblem = captureProblem_;

        // The speaker: a new clip, or the one playing told to stop.
        Finished ended;
        std::string speakerProblem = speakerProblem_;
        if (newClip)
        {
            ended = closePlayback();
            speakerProblem.clear();
            if ((request.samples.empty() && !request.open) || request.rate <= 0)
            {
                if (request.finished) request.finished(false);
            }
            else if (!speaker.usable())
            {
                speakerProblem = voice::describe(speaker, false);
                if (request.finished) request.finished(false);
            }
            else
            {
                speakerProblem = openPlayback(devices, speaker.node.name, std::move(request));
            }
        }
        else if (render_ && (!speaker.usable() || speaker.node.name != renderName_))
        {
            // Gone, or the radio's now: never another sound on it.
            ended = closePlayback();
            speakerProblem = speaker.usable() ? "The speaker changed" : voice::describe(speaker, false);
        }
        speakerProblem_ = speakerProblem;
        if (ended) ended(false);

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.devices = found;
            status_.chosenMicrophone = chosenMicrophone;
            status_.chosenSpeaker = chosenSpeaker;
            status_.microphone = microphone;
            status_.speaker = speaker;
            status_.listening = capture_ != nullptr;
            status_.microphoneRate = capture_ ? captureLayout_.rate : 0;
            status_.microphoneProblem = microphoneProblem;
            status_.playing = render_ != nullptr;
            status_.speakerProblem = speakerProblem;
        }
        notify();
    }

    //---------------------------------------------------------------------
    // The microphone
    //---------------------------------------------------------------------

    std::string openCapture(IMMDeviceEnumerator* devices, const std::string& name)
    {
        Com<IMMDevice> device;
        if (FAILED(devices->GetDevice(wide(name).c_str(), device.put()))) return "That microphone isn't there";
        Com<IAudioClient> client;
        if (FAILED(device->Activate(I_AUDIO_CLIENT, CLSCTX_ALL, nullptr, client.putVoid())))
            return "Windows wouldn't open the microphone";
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) return "The microphone didn't say its format";
        const Layout layout = layoutOf(mix);
        HRESULT hr = E_FAIL;
        if (layout.ok) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, BUFFER, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (!layout.ok) return "The microphone's format isn't one Ragtime reads";
        if (hr == AUDCLNT_E_DEVICE_IN_USE) return "Another program has the microphone to itself (exclusive mode)";
        if (FAILED(hr)) return "Windows wouldn't record from the microphone";
        Com<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(I_AUDIO_CAPTURE_CLIENT, capture.putVoid())) || FAILED(client->Start()))
            return "Windows wouldn't record from the microphone";
        captureClient_ = client.release();
        capture_ = capture.release();
        captureLayout_ = layout;
        captureName_ = name;
        captureProblem_.clear();
        lastCaptureMs_ = 0;
        return "";
    }

    void closeCapture()
    {
        if (captureClient_) captureClient_->Stop();
        if (capture_) capture_->Release();
        if (captureClient_) captureClient_->Release();
        capture_ = nullptr;
        captureClient_ = nullptr;
        captureName_.clear();
        captureLayout_ = Layout();
        lastCaptureMs_ = 0;
    }

    void pumpCapture()
    {
        Samples samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples = samples_;
        }
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
                const float share = 1.0f / (float)captureLayout_.channels;
                const int bytes = captureLayout_.bits / 8;
                for (UINT32 f = 0; f < frames; f++)
                {
                    const BYTE* frame = data + (std::size_t)f * (std::size_t)captureLayout_.stride;
                    float sum = 0;
                    for (int c = 0; c < captureLayout_.channels; c++)
                        sum += sampleAt(frame + c * bytes, captureLayout_);
                    mono_[f] = sum * share;
                }
            }
            capture_->ReleaseBuffer(frames);
            if (frames > 0 && samples) samples(mono_.data(), mono_.size(), captureLayout_.rate);
            lastCaptureMs_ = nowMs();
        }
        if (FAILED(hr))
        {
            closeCapture();
            captureProblem_ = hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The microphone went away" : "Recording stopped";
            std::lock_guard<std::mutex> lock(mutex_);
            poked_ = true;
        }
    }

    //---------------------------------------------------------------------
    // The speaker
    //---------------------------------------------------------------------

    // "" when it started; otherwise why not, with the clip's finished
    // called.
    std::string openPlayback(IMMDeviceEnumerator* devices, const std::string& name, Clip clip)
    {
        auto fail = [&](const char* why) {
            if (clip.finished) clip.finished(false);
            return std::string(why);
        };
        Com<IMMDevice> device;
        if (FAILED(devices->GetDevice(wide(name).c_str(), device.put()))) return fail("That speaker isn't there");
        Com<IAudioClient> client;
        if (FAILED(device->Activate(I_AUDIO_CLIENT, CLSCTX_ALL, nullptr, client.putVoid())))
            return fail("Windows wouldn't open the speaker");
        // The clip as it is, mono floats at its rate; the engine converts.
        WAVEFORMATEX format;
        std::memset(&format, 0, sizeof format);
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = (DWORD)clip.rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, BUFFER, 0, &format, nullptr);
        if (hr == AUDCLNT_E_DEVICE_IN_USE) return fail("Another program has the speaker to itself (exclusive mode)");
        if (FAILED(hr)) return fail("Windows wouldn't play to the speaker");
        UINT32 frames = 0;
        Com<IAudioRenderClient> render;
        if (FAILED(client->GetBufferSize(&frames)) || frames == 0 ||
            FAILED(client->GetService(I_AUDIO_RENDER_CLIENT, render.putVoid())))
            return fail("Windows wouldn't play to the speaker");
        renderClient_ = client.release();
        render_ = render.release();
        renderFrames_ = frames;
        renderName_ = name;
        clip_ = std::move(clip);
        position_ = 0;
        started_ = false;
        handed_ = 0;
        playingRate_ = clip_.rate;
        // Fill the buffer before starting, so it never begins empty.
        pumpPlayback();
        if (render_ && FAILED(renderClient_->Start()))
        {
            Finished finished = closePlayback();
            if (finished) finished(false);
            return "Windows wouldn't play to the speaker";
        }
        started_ = render_ != nullptr;
        return "";
    }

    // Stops the clip; returns its finished, to call when convenient.
    Finished closePlayback()
    {
        if (renderClient_) renderClient_->Stop();
        if (render_) render_->Release();
        if (renderClient_) renderClient_->Release();
        render_ = nullptr;
        renderClient_ = nullptr;
        renderName_.clear();
        renderFrames_ = 0;
        position_ = 0;
        handed_ = 0;
        playingRate_ = 0;
        Finished finished = std::exchange(clip_.finished, nullptr);
        clip_ = Clip();
        return finished;
    }

    void pumpPlayback()
    {
        if (clip_.open)
        {
            // What has come of a clip still coming.
            std::lock_guard<std::mutex> lock(mutex_);
            if (clip_.generation == playGeneration_)
            {
                clip_.samples.insert(clip_.samples.end(), appended_.begin(), appended_.end());
                appended_.clear();
                if (ended_) clip_.open = false;
                ended_ = false;
            }
        }
        UINT32 padding = 0;
        HRESULT hr = renderClient_->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr) && position_ >= clip_.samples.size() && clip_.open)
            return;     // waiting for more: the engine plays silence
        if (SUCCEEDED(hr) && position_ >= clip_.samples.size())
        {
            // All handed over: done once the engine has played it out.
            if (padding == 0 || !started_)
            {
                if (!started_ && padding != 0) return;
                Finished finished = closePlayback();
                {
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    status_.playing = false;
                }
                if (finished) finished(true);
                notify();
            }
            return;
        }
        if (SUCCEEDED(hr))
        {
            const UINT32 room = renderFrames_ - std::min(padding, renderFrames_);
            const UINT32 n = (UINT32)std::min<std::size_t>(room, clip_.samples.size() - position_);
            if (n == 0) return;
            BYTE* data = nullptr;
            hr = render_->GetBuffer(n, &data);
            if (SUCCEEDED(hr) && data)
            {
                const float gain = volume_.load();
                float* out = reinterpret_cast<float*>(data);
                for (UINT32 i = 0; i < n; i++) out[i] = clip_.samples[position_ + i] * gain;
                hr = render_->ReleaseBuffer(n, 0);
                position_ += n;
                handed_ = position_;
            }
        }
        if (FAILED(hr))
        {
            Finished finished = closePlayback();
            speakerProblem_ =
                hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The speaker went away" : "Windows stopped playing";
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.playing = false;
                status_.speakerProblem = speakerProblem_;
            }
            if (finished) finished(false);
            notify();
        }
    }

    void notify()
    {
        if (changed_) changed_();
    }

    Changed changed_;
    std::thread thread_;

    // Asked of the voice thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    bool poked_ = false;
    std::string chosenMicrophone_;
    std::string chosenSpeaker_;
    std::vector<std::string> remembered_;
    bool listening_ = false;
    uint64_t listenGeneration_ = 0;
    Samples samples_;
    Clip request_;
    uint64_t playGeneration_ = 0;
    bool streamOpen_ = false;           // the latest clip takes append()
    std::vector<float> appended_;       // for the clip playing, not yet taken
    bool ended_ = false;                // endStream() for the clip playing

    // The voice thread's own.
    ear::Graph graph_;
    std::map<std::string, uint32_t> ids_;
    std::string defaultSource_;
    std::string defaultSink_;
    IAudioClient* captureClient_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    Layout captureLayout_;
    std::string captureName_;
    uint64_t captureGeneration_ = 0;
    std::string captureProblem_;
    std::vector<float> mono_;
    IAudioClient* renderClient_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    UINT32 renderFrames_ = 0;
    std::string renderName_;
    Clip clip_;
    std::size_t position_ = 0;
    bool started_ = false;
    std::string speakerProblem_;
    std::atomic<int64_t> lastCaptureMs_{0};
    std::atomic<std::size_t> handed_{0};
    std::atomic<int> playingRate_{0};
    std::atomic<float> volume_{1.0f};

    mutable std::mutex statusMutex_;
    VoiceStatus status_;
};

} // namespace

std::unique_ptr<VoiceAudio> makeVoiceAudio()
{
    return std::make_unique<WasapiVoice>();
}

} // namespace audio
} // namespace ragtime
