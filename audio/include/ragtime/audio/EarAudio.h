// EarAudio: the Ear's ears. It listens, through PipeWire, to the sound
// card data2g-host records from, alongside it, and hands the samples to
// the Ear's spectrum (docs/STAGE1.md, "The Ear"; Jeff's call, 2026-10-26).
//
// It follows data2g-host by default: it watches PipeWire's graph for the
// host's capture (ear::findHostCapture) and records the source feeding
// it, moving when the host moves. The operator can pick any source
// instead. It never plays a sound, never opens ALSA's devices itself, and
// asks PipeWire for nothing the host would notice:
//
//   - a long latency, 100 ms (node.latency 4800/48000), so it can never
//     shorten the graph's cycle (the shortest any stream asks for wins);
//   - the source's own rate, so nothing resamples or switches rate;
//   - passive (node.passive), so it never keeps the sound card running:
//     when data2g-host stops, the Ear goes quiet with it (a source the
//     operator picks that the host doesn't use may be woken);
//   - pinned to its source (target.object, node.dont-reconnect), so the
//     session manager never moves it; it moves itself, following the host.
//
// libpipewire is loaded when the Ear first opens (dlopen), not linked, so
// Ragtime and its AppImage run on machines without PipeWire, where the
// Ear only says it needs it. On Windows a stand-in says the same.
//
// The callbacks come on PipeWire's thread.
#ifndef RAGTIME_AUDIO_EAR_AUDIO_H
#define RAGTIME_AUDIO_EAR_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/ear/Graph.h"

namespace ragtime
{
namespace audio
{

struct EarStatus
{
    // Connected to PipeWire; when not, problem says why.
    bool available = false;
    std::string problem;
    // The sources the dropdown offers.
    std::vector<ear::GraphNode> sources;
    // data2g-host's capture, as last seen.
    ear::HostSearch host;
    // Following data2g-host, or the source the operator picked.
    bool following = true;
    std::string chosen;
    // The source the Ear records, if any, and whether samples are coming.
    uint32_t listening = 0;
    std::string listeningName;
    std::string listeningDescription;
    bool streaming = false;
    int rate = 0;
};

class EarAudio
{
public:
    using Samples = std::function<void(const float* samples, std::size_t n, int rate)>;
    using Changed = std::function<void()>;

    virtual ~EarAudio() = default;

    // Connects to PipeWire and starts following. samples gets mono floats;
    // changed is called when status() changes.
    virtual void start(Samples samples, Changed changed) = 0;
    // Disconnects; no callbacks after it returns.
    virtual void stop() = 0;
    // Empty follows data2g-host; otherwise a source's node.name.
    virtual void choose(const std::string& sourceName) = 0;
    // The process listening on the host's command port, when known, to
    // tell several data2g-hosts apart.
    virtual void setHostPid(std::optional<int> pid) = 0;

    virtual EarStatus status() const = 0;
};

// PipeWire on Linux; a stand-in that says why not elsewhere.
std::unique_ptr<EarAudio> makeEarAudio();

} // namespace audio
} // namespace ragtime

#endif
