// EarAudio: the Ear's ears. It listens to the sound card data2g-host
// records from, alongside it, and hands the samples to the Ear's spectrum
// (docs/STAGE1.md, "The Ear"; Jeff's call, 2026-10-26). PipeWire on Linux,
// WASAPI on Windows (2026-10-27).
//
// It follows data2g-host by default: it watches for the host's capture
// (ear::findHostCapture) and records the source feeding it, moving when
// the host moves. With no host recording it stays on the card the host
// last used, or listens to the system's default input, so it always hears
// something, rig or no rig (ear::pickSource). The operator can pick any
// source instead. It never plays a sound.
//
// On PipeWire it never opens ALSA's devices itself, and asks for nothing
// the host would notice:
//
//   - a long latency, 100 ms (node.latency 4800/48000), so it can never
//     shorten the graph's cycle (the shortest any stream asks for wins);
//   - the source's own rate, so nothing resamples or switches rate;
//   - passive (node.passive) on the card the host records from, so it
//     never keeps that card running on the host's account; any other card
//     it wakes itself;
//   - pinned to its source (target.object, node.dont-reconnect), so the
//     session manager never moves it; it moves itself, following the host.
//
// libpipewire is loaded when the Ear first opens (dlopen), not linked, so
// Ragtime and its AppImage run on machines without PipeWire, where the
// Ear only says it needs it.
//
// On Windows it records in WASAPI's shared mode, as data2g-host's audio
// library (miniaudio) does by default, so the two share the card. Which
// input the host records from is read from the audio sessions on each
// input, by their process ids; a 100 ms buffer at the card's own format.
//
// The callbacks come on PipeWire's thread, or the Ear's own on Windows.
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
    // Connected to PipeWire (or WASAPI); when not, problem says why.
    bool available = false;
    std::string problem;
    // The sources the dropdown offers.
    std::vector<ear::GraphNode> sources;
    // data2g-host's capture, as last seen.
    ear::HostSearch host;
    // Following data2g-host, or the source the operator picked.
    bool following = true;
    std::string chosen;
    // Why the Ear records the source it does.
    ear::Pick pick = ear::Pick::None;
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

    // Connects to PipeWire (or WASAPI) and starts following. samples gets mono floats;
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

// PipeWire on Linux, WASAPI on Windows; a stand-in that says why not
// elsewhere.
std::unique_ptr<EarAudio> makeEarAudio();

// The process listening on a TCP port on this machine, if any we may see:
// /proc on Linux (ear::processListeningOn), the TCP table on Windows.
std::optional<int> processListeningOn(int port);

} // namespace audio
} // namespace ragtime

#endif
