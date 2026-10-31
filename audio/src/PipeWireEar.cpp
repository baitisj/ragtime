// EarAudio on PipeWire, with libpipewire loaded at run time (EarAudio.h).
#include "ragtime/audio/EarAudio.h"

#include "PipeWireApi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
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

class PipeWireEar : public EarAudio
{
public:
    ~PipeWireEar() override { stop(); }

    void start(Samples samples, Changed changed) override
    {
        stop();
        samples_ = std::move(samples);
        changed_ = std::move(changed);
        PipeWireApi& pw = api();
        std::string problem = pw.load();
        if (problem.empty())
        {
            loop_ = pw.threadLoopNew("ragtime-ear", nullptr);
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
            // The core itself failing means the daemon went away.
            if (id == PW_ID_CORE && res == -EPIPE)
                static_cast<PipeWireEar*>(self)->setProblem(std::string("PipeWire stopped: ") + (message ? message : ""));
        };
        std::memset(&registryEvents_, 0, sizeof registryEvents_);
        registryEvents_.version = PW_VERSION_REGISTRY_EVENTS;
        registryEvents_.global = [](void* self, uint32_t id, uint32_t, const char* type, uint32_t,
                                    const spa_dict* props) { static_cast<PipeWireEar*>(self)->global(id, type, props); };
        registryEvents_.global_remove = [](void* self, uint32_t id) { static_cast<PipeWireEar*>(self)->remove(id); };

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
        teardown();
        samples_ = nullptr;
        changed_ = nullptr;
    }

    void choose(const std::string& sourceName) override
    {
        locked([&] {
            chosen_ = sourceName;
            evaluate();
        });
    }

    void setHostPid(std::optional<int> pid) override
    {
        locked([&] {
            if (pid == hostPid_) return;
            hostPid_ = pid;
            evaluate();
        });
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
        destroyStream();
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
        lastHostSource_.clear();
        defaultSource_.clear();
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = EarStatus();
    }

    void setProblem(const std::string& problem)
    {
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.available = false;
            status_.problem = problem;
            status_.streaming = false;
        }
        notify();
    }

    void notify()
    {
        if (changed_) changed_();
    }

    // The graph, from the registry: nodes, the clients owning them, links.
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
            // The session manager's "default" metadata names the default
            // input, the Ear's last resort.
            if (metadata_ || lookup(props, PW_KEY_METADATA_NAME) != "default") return;
            metadata_ = static_cast<pw_metadata*>(
                pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (!metadata_) return;
            std::memset(&metadataEvents_, 0, sizeof metadataEvents_);
            metadataEvents_.version = PW_VERSION_METADATA_EVENTS;
            metadataEvents_.property = [](void* self, uint32_t subject, const char* key, const char*,
                                          const char* value) {
                static_cast<PipeWireEar*>(self)->property(subject, key, value);
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
        evaluate();
    }

    void property(uint32_t subject, const char* key, const char* value)
    {
        // A null key clears them all.
        if (subject != PW_ID_CORE || (key && std::strcmp(key, "default.audio.source") != 0)) return;
        const std::string name = jsonName(value);
        if (name == defaultSource_) return;
        defaultSource_ = name;
        evaluate();
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
        evaluate();
    }

    // Which source to record, and (re)connecting to it when that changes.
    void evaluate()
    {
        ear::HostSearch host = ear::findHostCapture(graph_, hostPid_);
        if (host.found && host.found->source != 0 && graph_.node(host.found->source))
            lastHostSource_ = host.found->sourceName;
        const ear::SourcePick pick = ear::pickSource(graph_, chosen_, host, lastHostSource_, defaultSource_);
        const ear::GraphNode* want = pick.node;
        const bool passive = pick.passive;

        const uint32_t wantId = want ? want->id : 0;
        if (wantId != target_ || (want && passive != passive_))
        {
            {
                // A refusal is the last stream's; the new one may be fine.
                std::lock_guard<std::mutex> lock(statusMutex_);
                status_.problem.clear();
            }
            destroyStream();
            if (want) createStream(*want, passive);
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.sources = ear::sources(graph_);
            status_.host = host;
            status_.following = chosen_.empty();
            status_.chosen = chosen_;
            status_.pick = target_ != 0 ? pick.why : ear::Pick::None;
            status_.listening = target_;
            status_.listeningName = want ? want->name : "";
            status_.listeningDescription = want ? (want->description.empty() ? want->name : want->description) : "";
            status_.rate = rate_;
        }
        // Every graph event lands here; the window redraws only what differs.
        notify();
    }

    void createStream(const ear::GraphNode& source, bool passive)
    {
        PipeWireApi& pw = api();
        const std::string id = std::to_string(source.id);
        pw_properties* props = pw.propertiesNew(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_APP_NAME, "Ragtime", PW_KEY_NODE_NAME, "ragtime-ear", PW_KEY_NODE_DESCRIPTION, "Ragtime Ear",
            PW_KEY_NODE_LATENCY, "4800/48000", "node.passive", passive ? "true" : "false", "target.object", source.name.c_str(),
            "node.target", id.c_str(), "node.dont-reconnect", "true", PW_KEY_NODE_AUTOCONNECT, "true", nullptr);
        stream_ = pw.streamNew(core_, "Ragtime Ear", props);
        if (!stream_) return;

        std::memset(&streamEvents_, 0, sizeof streamEvents_);
        streamEvents_.version = PW_VERSION_STREAM_EVENTS;
        streamEvents_.process = [](void* self) { static_cast<PipeWireEar*>(self)->process(); };
        streamEvents_.param_changed = [](void* self, uint32_t paramId, const spa_pod* param) {
            static_cast<PipeWireEar*>(self)->paramChanged(paramId, param);
        };
        streamEvents_.state_changed = [](void* self, pw_stream_state, pw_stream_state state, const char*) {
            static_cast<PipeWireEar*>(self)->stateChanged(state);
        };
        std::memset(&streamHook_, 0, sizeof streamHook_);
        pw.streamAddListener(stream_, &streamHook_, &streamEvents_, this);

        // Mono floats at whatever rate the source runs: no rate asked for.
        uint8_t buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof buffer);
        spa_audio_info_raw info;
        std::memset(&info, 0, sizeof info);
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        const int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT;
        if (pw.streamConnect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, (pw_stream_flags)flags, params, 1) < 0)
        {
            destroyStream();
            return;
        }
        target_ = source.id;
        passive_ = passive;
    }

    void destroyStream()
    {
        if (stream_)
        {
            spa_hook_remove(&streamHook_);
            api().streamDestroy(stream_);
            stream_ = nullptr;
        }
        target_ = 0;
        rate_ = 0;
        lastSamplesMs_ = 0;
    }

    void paramChanged(uint32_t id, const spa_pod* param)
    {
        if (id != SPA_PARAM_Format || !param) return;
        spa_audio_info_raw info;
        std::memset(&info, 0, sizeof info);
        if (spa_format_audio_raw_parse(param, &info) < 0) return;
        rate_ = (int)info.rate;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.rate = rate_;
    }

    void stateChanged(pw_stream_state state)
    {
        // A passive stream can sit "paused" while buffers flow, so whether
        // audio is coming is judged by the buffers (status()); only an
        // error is taken from the state.
        if (state != PW_STREAM_STATE_ERROR) return;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.problem = "PipeWire refused the Ear's recording";
        }
        notify();
    }

    void process()
    {
        PipeWireApi& pw = api();
        pw_buffer* b = pw.streamDequeueBuffer(stream_);
        if (!b) return;
        spa_buffer* buf = b->buffer;
        if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk && rate_ > 0 && samples_)
        {
            const spa_data& d = buf->datas[0];
            const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
            samples_(reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + offset),
                     size / sizeof(float), rate_);
            lastSamplesMs_ = nowMs();
        }
        pw.streamQueueBuffer(stream_, b);
    }

    Samples samples_;
    Changed changed_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_stream* stream_ = nullptr;
    pw_metadata* metadata_ = nullptr;
    spa_hook metadataHook_{};
    pw_metadata_events metadataEvents_{};
    spa_hook coreHook_{};
    spa_hook registryHook_{};
    spa_hook streamHook_{};
    pw_core_events coreEvents_{};
    pw_registry_events registryEvents_{};
    pw_stream_events streamEvents_{};

    // Touched only on PipeWire's thread, or with its loop locked.
    ear::Graph graph_;
    std::vector<uint32_t> links_;   // the link ids, beside graph_.links
    std::string chosen_;
    std::string lastHostSource_;    // the source data2g-host last used
    std::string defaultSource_;     // the default input's node.name
    std::optional<int> hostPid_;
    uint32_t target_ = 0;
    bool passive_ = true;
    int rate_ = 0;
    std::atomic<int64_t> lastSamplesMs_{0};

    mutable std::mutex statusMutex_;
    EarStatus status_;
};

} // namespace

std::unique_ptr<EarAudio> makeEarAudio()
{
    return std::make_unique<PipeWireEar>();
}

} // namespace audio
} // namespace ragtime
