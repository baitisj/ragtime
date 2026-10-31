// VoiceAudio on PipeWire, with libpipewire loaded at run time
// (VoiceAudio.h). One thread loop of its own: the registry gives the graph
// (sources, sinks, the programs' streams and the links between them) and
// the default metadata the system's default input and output; from those
// voice::devices leaves out the radio's cards. The microphone's stream
// exists only while listening, the speaker's only while a clip plays, each
// pinned to its device (target.object, node.dont-reconnect), so the
// session manager never moves it, least of all onto the radio.
#include "ragtime/audio/VoiceAudio.h"

#include "PipeWireApi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>

namespace ragtime
{
namespace audio
{

namespace
{

using namespace pipewire;

// The microphone's buffer, 50 ms: short enough for lively meters. The
// speaker's, 100 ms: a clip has no hurry.
constexpr const char* CAPTURE_LATENCY = "2400/48000";
constexpr const char* PLAYBACK_LATENCY = "4800/48000";
// Frames written a cycle at most, at the clip's rate.
constexpr std::size_t MOST_FRAMES = 2048;

class PipeWireVoice : public VoiceAudio
{
public:
    ~PipeWireVoice() override { stop(); }

    void start(Changed changed, const std::vector<std::string>& remembered) override
    {
        stop();
        changed_ = std::move(changed);
        remembered_ = voice::rememberRadio({}, remembered);
        PipeWireApi& pw = api();
        std::string problem = pw.load();
        if (problem.empty())
        {
            loop_ = pw.threadLoopNew("ragtime-voice", nullptr);
            if (loop_) context_ = pw.contextNew(pw.threadLoopGetLoop(loop_), nullptr, 0);
            if (context_) core_ = pw.contextConnect(context_, nullptr, 0);
            if (!core_) problem = "PipeWire isn't running";
        }
        if (!problem.empty())
        {
            teardown();
            setProblem(problem);
            return;
        }

        std::memset(&coreEvents_, 0, sizeof coreEvents_);
        coreEvents_.version = PW_VERSION_CORE_EVENTS;
        coreEvents_.error = [](void* self, uint32_t id, int, int res, const char* message) {
            if (id == PW_ID_CORE && res == -EPIPE)
                static_cast<PipeWireVoice*>(self)->setProblem(std::string("PipeWire stopped: ") +
                                                              (message ? message : ""));
        };
        std::memset(&registryEvents_, 0, sizeof registryEvents_);
        registryEvents_.version = PW_VERSION_REGISTRY_EVENTS;
        registryEvents_.global = [](void* self, uint32_t id, uint32_t, const char* type, uint32_t,
                                    const spa_dict* props) {
            static_cast<PipeWireVoice*>(self)->global(id, type, props);
        };
        registryEvents_.global_remove = [](void* self, uint32_t id) { static_cast<PipeWireVoice*>(self)->remove(id); };

        pw.threadLoopLock(loop_);
        std::memset(&coreHook_, 0, sizeof coreHook_);
        pw_core_add_listener(core_, &coreHook_, &coreEvents_, this);
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        std::memset(&registryHook_, 0, sizeof registryHook_);
        pw_registry_add_listener(registry_, &registryHook_, &registryEvents_, this);
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.available = true;
            status_.problem.clear();
        }
        pw.threadLoopStart(loop_);
        pw.threadLoopUnlock(loop_);
        notify();
    }

    void stop() override
    {
        Finished finished;
        locked([&] { finished = endPlayback(); });
        teardown();
        if (finished) finished(false);
        changed_ = nullptr;
    }

    void chooseMicrophone(const std::string& name) override
    {
        locked([&] {
            chosenMicrophone_ = name;
            evaluate();
        });
    }

    void chooseSpeaker(const std::string& name) override
    {
        Finished finished;
        locked([&] {
            chosenSpeaker_ = name;
            finished = evaluate();
        });
        if (finished) finished(false);
    }

    void forgetRadio() override
    {
        locked([&] {
            remembered_.clear();
            evaluate();
        });
    }

    void listen(Samples samples) override
    {
        locked([&] {
            samples_ = std::move(samples);
            listening_ = true;
            destroyCapture();
            evaluate();
        });
    }

    void stopListening() override
    {
        locked([&] {
            listening_ = false;
            destroyCapture();
            samples_ = nullptr;
            evaluate();
        });
    }

    void play(std::vector<float> samples, int rate, Finished finished) override
    {
        begin(std::move(samples), rate, false, std::move(finished));
    }

    void playStream(int rate, Finished finished) override { begin({}, rate, true, std::move(finished)); }

    void append(const float* samples, std::size_t n) override
    {
        locked([&] {
            if (open_ && samples) clip_.insert(clip_.end(), samples, samples + n);
        });
    }

    void endStream() override
    {
        locked([&] { open_ = false; });
    }

private:
    void begin(std::vector<float> samples, int rate, bool open, Finished finished)
    {
        Finished before;
        locked([&] {
            before = endPlayback();
            clip_ = std::move(samples);
            clipRate_ = rate;
            open_ = open;
            position_ = 0;
            finished_ = std::move(finished);
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.speakerProblem.clear();
            }
            if (!core_ || rate <= 0 || (clip_.empty() && !open_))
                failPlayback(core_ ? "Nothing to play" : "No sound: PipeWire isn't connected");
            else if (!speaker_.usable())
                failPlayback(voice::describe(speaker_, false));
            else
                createPlayback(speaker_.node);
            publish();
        });
        if (before) before(false);
        // A clip that couldn't start ends at once.
        Finished failed;
        locked([&] {
            if (!playback_ && finished_) failed = std::exchange(finished_, nullptr);
        });
        if (failed) failed(false);
        notify();
    }

public:
    void stopPlaying() override
    {
        Finished finished;
        locked([&] {
            finished = endPlayback();
            publish();
        });
        if (finished) finished(false);
        notify();
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
    template <typename Fn>
    void locked(Fn fn)
    {
        if (!loop_)
        {
            fn();
            return;
        }
        api().threadLoopLock(loop_);
        fn();
        api().threadLoopUnlock(loop_);
    }

    void teardown()
    {
        PipeWireApi& pw = api();
        if (loop_) pw.threadLoopStop(loop_);
        destroyCapture();
        destroyPlayback();
        if (metadata_)
        {
            spa_hook_remove(&metadataHook_);
            pw.proxyDestroy(reinterpret_cast<pw_proxy*>(metadata_));
            metadata_ = nullptr;
        }
        if (registry_)
        {
            spa_hook_remove(&registryHook_);
            pw.proxyDestroy(reinterpret_cast<pw_proxy*>(registry_));
            registry_ = nullptr;
        }
        if (core_)
        {
            spa_hook_remove(&coreHook_);
            pw.coreDisconnect(core_);
            core_ = nullptr;
        }
        if (context_)
        {
            pw.contextDestroy(context_);
            context_ = nullptr;
        }
        if (loop_)
        {
            pw.threadLoopDestroy(loop_);
            loop_ = nullptr;
        }
        graph_ = ear::Graph();
        links_.clear();
        defaultSource_.clear();
        defaultSink_.clear();
        listening_ = false;
        samples_ = nullptr;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = VoiceStatus();
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

    void global(uint32_t id, const char* type, const spa_dict* props)
    {
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
        {
            ear::GraphNode n;
            n.id = id;
            n.clientId = number(lookup(props, PW_KEY_CLIENT_ID));
            n.name = lookup(props, PW_KEY_NODE_NAME);
            n.description = lookup(props, PW_KEY_NODE_DESCRIPTION);
            if (n.description.empty()) n.description = lookup(props, PW_KEY_NODE_NICK);
            n.mediaClass = lookup(props, PW_KEY_MEDIA_CLASS);
            n.appName = lookup(props, PW_KEY_APP_NAME);
            n.target = lookup(props, "target.object");
            if (n.target.empty()) n.target = lookup(props, "node.target");
            n.device = lookup(props, PW_KEY_DEVICE_ID);
            graph_.nodes.push_back(n);
        }
        else if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0)
        {
            ear::GraphClient c;
            c.id = id;
            c.appName = lookup(props, PW_KEY_APP_NAME);
            c.pid = (int)number(lookup(props, PW_KEY_SEC_PID));
            graph_.clients.push_back(c);
        }
        else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0)
        {
            ear::GraphLink l;
            l.outputNode = number(lookup(props, PW_KEY_LINK_OUTPUT_NODE));
            l.inputNode = number(lookup(props, PW_KEY_LINK_INPUT_NODE));
            links_.push_back(id);
            graph_.links.push_back(l);
        }
        else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
        {
            if (metadata_ || lookup(props, PW_KEY_METADATA_NAME) != "default") return;
            metadata_ = static_cast<pw_metadata*>(
                pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (!metadata_) return;
            std::memset(&metadataEvents_, 0, sizeof metadataEvents_);
            metadataEvents_.version = PW_VERSION_METADATA_EVENTS;
            metadataEvents_.property = [](void* self, uint32_t subject, const char* key, const char*,
                                          const char* value) {
                static_cast<PipeWireVoice*>(self)->property(subject, key, value);
                return 0;
            };
            std::memset(&metadataHook_, 0, sizeof metadataHook_);
            pw_metadata_add_listener(metadata_, &metadataHook_, &metadataEvents_, this);
            return;
        }
        else
        {
            return;
        }
        reevaluate();
    }

    void property(uint32_t subject, const char* key, const char* value)
    {
        if (subject != PW_ID_CORE) return;
        // A null key clears them all.
        const bool source = !key || std::strcmp(key, "default.audio.source") == 0;
        const bool sink = !key || std::strcmp(key, "default.audio.sink") == 0;
        if (!source && !sink) return;
        const std::string name = jsonName(value);
        if (source) defaultSource_ = name;
        if (sink) defaultSink_ = name;
        reevaluate();
    }

    void remove(uint32_t id)
    {
        auto& nodes = graph_.nodes;
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const ear::GraphNode& n) { return n.id == id; }),
                    nodes.end());
        auto& clients = graph_.clients;
        clients.erase(
            std::remove_if(clients.begin(), clients.end(), [&](const ear::GraphClient& c) { return c.id == id; }),
            clients.end());
        for (std::size_t i = 0; i < links_.size(); i++)
            if (links_[i] == id)
            {
                links_.erase(links_.begin() + (long)i);
                graph_.links.erase(graph_.links.begin() + (long)i);
                break;
            }
        reevaluate();
    }

    // evaluate() on PipeWire's thread, where a clip it ends is finished
    // there too.
    void reevaluate()
    {
        Finished finished = evaluate();
        if (finished) finished(false);
    }

    // Which devices, and following them: a new microphone reopens, a
    // speaker that isn't the pick any more (gone, or now the radio's)
    // stops the clip. Returns the finished of a clip it stopped.
    Finished evaluate()
    {
        devices_ = voice::devices(graph_, remembered_);
        remembered_ = voice::rememberRadio(remembered_, devices_.radioNow);
        microphone_ = voice::pickDevice(devices_.microphones, devices_, chosenMicrophone_, defaultSource_);
        speaker_ = voice::pickDevice(devices_.speakers, devices_, chosenSpeaker_, defaultSink_);

        if (!listening_ || !microphone_.usable())
            destroyCapture();
        else if (microphone_.node.id != captureTarget_)
        {
            destroyCapture();
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.microphoneProblem.clear();
            }
            createCapture(microphone_.node);
        }

        Finished finished;
        if (playback_ && (!speaker_.usable() || speaker_.node.id != playbackTarget_))
        {
            finished = endPlayback();
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.speakerProblem = speaker_.usable() ? "The speaker changed" : voice::describe(speaker_, false);
        }
        publish();
        notify();
        return finished;
    }

    void publish()
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.devices = devices_;
        status_.chosenMicrophone = chosenMicrophone_;
        status_.chosenSpeaker = chosenSpeaker_;
        status_.microphone = microphone_;
        status_.speaker = speaker_;
        status_.listening = listening_ && capture_ != nullptr;
        status_.microphoneRate = captureRate_;
        if (listening_ && !microphone_.usable()) status_.microphoneProblem = voice::describe(microphone_, true);
        status_.playing = playback_ != nullptr && !drained_;
    }

    static const spa_pod* monoFormat(spa_pod_builder* builder, int rate)
    {
        spa_audio_info_raw info;
        std::memset(&info, 0, sizeof info);
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        info.rate = (uint32_t)std::max(0, rate);
        return spa_format_audio_raw_build(builder, SPA_PARAM_EnumFormat, &info);
    }

    //---------------------------------------------------------------------
    // The microphone
    //---------------------------------------------------------------------

    void createCapture(const ear::GraphNode& source)
    {
        PipeWireApi& pw = api();
        const std::string id = std::to_string(source.id);
        pw_properties* props = pw.propertiesNew(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_APP_NAME, "Ragtime", PW_KEY_NODE_NAME, "ragtime-microphone", PW_KEY_NODE_DESCRIPTION,
            "Ragtime microphone", PW_KEY_NODE_LATENCY, CAPTURE_LATENCY, "target.object", source.name.c_str(),
            "node.target", id.c_str(), "node.dont-reconnect", "true", PW_KEY_NODE_AUTOCONNECT, "true", nullptr);
        capture_ = pw.streamNew(core_, "Ragtime microphone", props);
        if (!capture_) return;

        std::memset(&captureEvents_, 0, sizeof captureEvents_);
        captureEvents_.version = PW_VERSION_STREAM_EVENTS;
        captureEvents_.process = [](void* self) { static_cast<PipeWireVoice*>(self)->captured(); };
        captureEvents_.param_changed = [](void* self, uint32_t paramId, const spa_pod* param) {
            static_cast<PipeWireVoice*>(self)->captureFormat(paramId, param);
        };
        captureEvents_.state_changed = [](void* self, pw_stream_state, pw_stream_state state, const char* error) {
            if (state == PW_STREAM_STATE_ERROR)
                static_cast<PipeWireVoice*>(self)->captureFailed(error ? error : "");
        };
        std::memset(&captureHook_, 0, sizeof captureHook_);
        pw.streamAddListener(capture_, &captureHook_, &captureEvents_, this);

        // Mono floats at the microphone's own rate: none asked for.
        uint8_t buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof buffer);
        const spa_pod* params[1] = {monoFormat(&builder, 0)};
        const int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT;
        if (pw.streamConnect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY, (pw_stream_flags)flags, params, 1) < 0)
        {
            destroyCapture();
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.microphoneProblem = "PipeWire wouldn't open the microphone";
            return;
        }
        captureTarget_ = source.id;
    }

    void destroyCapture()
    {
        if (capture_)
        {
            spa_hook_remove(&captureHook_);
            api().streamDestroy(capture_);
            capture_ = nullptr;
        }
        captureTarget_ = 0;
        captureRate_ = 0;
        lastCaptureMs_ = 0;
    }

    void captureFormat(uint32_t id, const spa_pod* param)
    {
        if (id != SPA_PARAM_Format || !param) return;
        spa_audio_info_raw info;
        std::memset(&info, 0, sizeof info);
        if (spa_format_audio_raw_parse(param, &info) < 0) return;
        captureRate_ = (int)info.rate;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.microphoneRate = captureRate_;
    }

    void captureFailed(const std::string& error)
    {
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.microphoneProblem = "PipeWire refused the microphone" + (error.empty() ? "" : ": " + error);
        }
        notify();
    }

    void captured()
    {
        PipeWireApi& pw = api();
        pw_buffer* b = pw.streamDequeueBuffer(capture_);
        if (!b) return;
        spa_buffer* buf = b->buffer;
        if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk && captureRate_ > 0 && samples_)
        {
            const spa_data& d = buf->datas[0];
            const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
            samples_(reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + offset),
                     size / sizeof(float), captureRate_);
            lastCaptureMs_ = nowMs();
        }
        pw.streamQueueBuffer(capture_, b);
    }

    //---------------------------------------------------------------------
    // The speaker
    //---------------------------------------------------------------------

    void createPlayback(const ear::GraphNode& sink)
    {
        PipeWireApi& pw = api();
        const std::string id = std::to_string(sink.id);
        pw_properties* props = pw.propertiesNew(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_APP_NAME, "Ragtime", PW_KEY_NODE_NAME, "ragtime-speaker", PW_KEY_NODE_DESCRIPTION,
            "Ragtime voice", PW_KEY_NODE_LATENCY, PLAYBACK_LATENCY, "target.object", sink.name.c_str(),
            "node.target", id.c_str(), "node.dont-reconnect", "true", PW_KEY_NODE_AUTOCONNECT, "true", nullptr);
        playback_ = pw.streamNew(core_, "Ragtime voice", props);
        if (!playback_)
        {
            failPlayback("PipeWire wouldn't open the speaker");
            return;
        }
        std::memset(&playbackEvents_, 0, sizeof playbackEvents_);
        playbackEvents_.version = PW_VERSION_STREAM_EVENTS;
        playbackEvents_.process = [](void* self) { static_cast<PipeWireVoice*>(self)->fill(); };
        playbackEvents_.drained = [](void* self) { static_cast<PipeWireVoice*>(self)->drained(); };
        playbackEvents_.state_changed = [](void* self, pw_stream_state, pw_stream_state state, const char* error) {
            if (state == PW_STREAM_STATE_ERROR)
                static_cast<PipeWireVoice*>(self)->playbackFailed(error ? error : "");
        };
        std::memset(&playbackHook_, 0, sizeof playbackHook_);
        pw.streamAddListener(playback_, &playbackHook_, &playbackEvents_, this);

        // The clip's own rate: PipeWire converts it to the speaker's.
        uint8_t buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof buffer);
        const spa_pod* params[1] = {monoFormat(&builder, clipRate_)};
        const int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT;
        if (pw.streamConnect(playback_, PW_DIRECTION_OUTPUT, PW_ID_ANY, (pw_stream_flags)flags, params, 1) < 0)
        {
            destroyPlayback();
            failPlayback("PipeWire wouldn't play to the speaker");
            return;
        }
        playbackTarget_ = sink.id;
        drained_ = false;
        flushed_ = false;
        handed_ = 0;
        playingRate_ = clipRate_;
    }

    void destroyPlayback()
    {
        if (playback_)
        {
            spa_hook_remove(&playbackHook_);
            api().streamDestroy(playback_);
            playback_ = nullptr;
        }
        playbackTarget_ = 0;
        drained_ = false;
        flushed_ = false;
    }

    // Stops the clip; returns its finished, to call outside the lock.
    Finished endPlayback()
    {
        destroyPlayback();
        clip_.clear();
        open_ = false;
        position_ = 0;
        handed_ = 0;
        playingRate_ = 0;
        return std::exchange(finished_, nullptr);
    }

    void failPlayback(const std::string& why)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.speakerProblem = why;
    }

    void fill()
    {
        PipeWireApi& pw = api();
        if (!playback_ || flushed_) return;
        if (position_ >= clip_.size() && !open_)
        {
            // All written: drained says when it has all been heard.
            flushed_ = true;
            pw.streamFlush(playback_, true);
            return;
        }
        pw_buffer* b = pw.streamDequeueBuffer(playback_);
        if (!b) return;
        spa_buffer* buf = b->buffer;
        if (!buf || buf->n_datas == 0 || !buf->datas[0].data)
        {
            pw.streamQueueBuffer(playback_, b);
            return;
        }
        spa_data& d = buf->datas[0];
        const std::size_t room = std::min<std::size_t>(d.maxsize / sizeof(float), MOST_FRAMES);
        float* out = static_cast<float*>(d.data);
        std::size_t n = std::min(room, clip_.size() - position_);
        if (n > 0)
        {
            const float gain = volume_.load();
            for (std::size_t i = 0; i < n; i++) out[i] = clip_[position_ + i] * gain;
            position_ += n;
            handed_ = position_;
        }
        else
        {
            // Waiting for more of a clip still coming: 20 ms of silence.
            n = std::min(room, (std::size_t)std::max(1, clipRate_ / 50));
            std::memset(out, 0, n * sizeof(float));
        }
        d.chunk->offset = 0;
        d.chunk->stride = sizeof(float);
        d.chunk->size = (uint32_t)(n * sizeof(float));
        pw.streamQueueBuffer(playback_, b);
        if (position_ >= clip_.size() && !open_)
        {
            flushed_ = true;
            pw.streamFlush(playback_, true);
        }
    }

    void drained()
    {
        if (!playback_ || drained_) return;
        drained_ = true;
        api().streamSetActive(playback_, false);
        Finished finished = std::exchange(finished_, nullptr);
        publish();
        if (finished) finished(true);
        notify();
    }

    void playbackFailed(const std::string& error)
    {
        failPlayback("PipeWire couldn't play to the speaker" + (error.empty() ? "" : ": " + error));
        // The stream goes on the next play or stop, never inside its own
        // callback.
        drained_ = true;
        Finished finished = std::exchange(finished_, nullptr);
        publish();
        if (finished) finished(false);
        notify();
    }

    Changed changed_;
    Samples samples_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_metadata* metadata_ = nullptr;
    pw_stream* capture_ = nullptr;
    pw_stream* playback_ = nullptr;
    spa_hook coreHook_{};
    spa_hook registryHook_{};
    spa_hook metadataHook_{};
    spa_hook captureHook_{};
    spa_hook playbackHook_{};
    pw_core_events coreEvents_{};
    pw_registry_events registryEvents_{};
    pw_metadata_events metadataEvents_{};
    pw_stream_events captureEvents_{};
    pw_stream_events playbackEvents_{};

    // Touched only on PipeWire's thread, or with its loop locked.
    ear::Graph graph_;
    std::vector<uint32_t> links_;
    std::string defaultSource_;
    std::string defaultSink_;
    std::vector<std::string> remembered_;
    std::string chosenMicrophone_;
    std::string chosenSpeaker_;
    voice::Devices devices_;
    voice::DevicePick microphone_;
    voice::DevicePick speaker_;
    bool listening_ = false;
    uint32_t captureTarget_ = 0;
    int captureRate_ = 0;
    uint32_t playbackTarget_ = 0;
    std::vector<float> clip_;
    bool open_ = false;     // more of the clip is coming (playStream)
    int clipRate_ = 0;
    std::size_t position_ = 0;
    bool flushed_ = false;
    bool drained_ = false;
    Finished finished_;
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
    return std::make_unique<PipeWireVoice>();
}

} // namespace audio
} // namespace ragtime
