// The libpipewire calls the Ear and voice make, loaded at run time
// (dlopen), never linked, and the small helpers both use. Internal to
// ragtime_audio.
#ifndef RAGTIME_AUDIO_PIPEWIRE_API_H
#define RAGTIME_AUDIO_PIPEWIRE_API_H

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

namespace ragtime
{
namespace audio
{
namespace pipewire
{

// The few libpipewire calls Ragtime makes; everything else it uses is
// inline in the headers.
struct PipeWireApi
{
    void* library = nullptr;
    decltype(&::pw_init) init = nullptr;
    decltype(&::pw_thread_loop_new) threadLoopNew = nullptr;
    decltype(&::pw_thread_loop_get_loop) threadLoopGetLoop = nullptr;
    decltype(&::pw_thread_loop_start) threadLoopStart = nullptr;
    decltype(&::pw_thread_loop_stop) threadLoopStop = nullptr;
    decltype(&::pw_thread_loop_lock) threadLoopLock = nullptr;
    decltype(&::pw_thread_loop_unlock) threadLoopUnlock = nullptr;
    decltype(&::pw_thread_loop_destroy) threadLoopDestroy = nullptr;
    decltype(&::pw_context_new) contextNew = nullptr;
    decltype(&::pw_context_connect) contextConnect = nullptr;
    decltype(&::pw_context_destroy) contextDestroy = nullptr;
    decltype(&::pw_core_disconnect) coreDisconnect = nullptr;
    decltype(&::pw_proxy_destroy) proxyDestroy = nullptr;
    decltype(&::pw_properties_new) propertiesNew = nullptr;
    decltype(&::pw_stream_new) streamNew = nullptr;
    decltype(&::pw_stream_add_listener) streamAddListener = nullptr;
    decltype(&::pw_stream_connect) streamConnect = nullptr;
    decltype(&::pw_stream_destroy) streamDestroy = nullptr;
    decltype(&::pw_stream_dequeue_buffer) streamDequeueBuffer = nullptr;
    decltype(&::pw_stream_queue_buffer) streamQueueBuffer = nullptr;
    decltype(&::pw_stream_flush) streamFlush = nullptr;
    decltype(&::pw_stream_set_active) streamSetActive = nullptr;

    // Loads it once; "" when it worked, or why not.
    std::string load()
    {
        if (library) return "";
        void* lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return "PipeWire isn't installed (no libpipewire-0.3.so.0)";
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (!fn) ok = false;
        };
        get(init, "pw_init");
        get(threadLoopNew, "pw_thread_loop_new");
        get(threadLoopGetLoop, "pw_thread_loop_get_loop");
        get(threadLoopStart, "pw_thread_loop_start");
        get(threadLoopStop, "pw_thread_loop_stop");
        get(threadLoopLock, "pw_thread_loop_lock");
        get(threadLoopUnlock, "pw_thread_loop_unlock");
        get(threadLoopDestroy, "pw_thread_loop_destroy");
        get(contextNew, "pw_context_new");
        get(contextConnect, "pw_context_connect");
        get(contextDestroy, "pw_context_destroy");
        get(coreDisconnect, "pw_core_disconnect");
        get(proxyDestroy, "pw_proxy_destroy");
        get(propertiesNew, "pw_properties_new");
        get(streamNew, "pw_stream_new");
        get(streamAddListener, "pw_stream_add_listener");
        get(streamConnect, "pw_stream_connect");
        get(streamDestroy, "pw_stream_destroy");
        get(streamDequeueBuffer, "pw_stream_dequeue_buffer");
        get(streamQueueBuffer, "pw_stream_queue_buffer");
        get(streamFlush, "pw_stream_flush");
        get(streamSetActive, "pw_stream_set_active");
        if (!ok)
        {
            dlclose(lib);
            return "This libpipewire is too old for Ragtime";
        }
        library = lib;
        init(nullptr, nullptr);
        return "";
    }
};

inline PipeWireApi& api()
{
    static PipeWireApi pw;
    return pw;
}

inline std::string lookup(const spa_dict* props, const char* key)
{
    const char* v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? v : "";
}

inline int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline uint32_t number(const std::string& text)
{
    return text.empty() ? 0u : (uint32_t)std::strtoul(text.c_str(), nullptr, 10);
}

// The name in the default metadata's JSON, {"name":"alsa_input..."}.
inline std::string jsonName(const char* value)
{
    if (!value) return "";
    const std::string v = value;
    const std::size_t key = v.find("\"name\"");
    if (key == std::string::npos) return "";
    const std::size_t open = v.find('"', v.find(':', key));
    if (open == std::string::npos) return "";
    const std::size_t close = v.find('"', open + 1);
    return close == std::string::npos ? "" : v.substr(open + 1, close - open - 1);
}

} // namespace pipewire
} // namespace audio
} // namespace ragtime

#endif
