// Devices: the microphones and speakers voice messages may use, with the
// radio's sound card left out (docs/STAGE1.md, "Voice audio").
//
// Ragtime never plays to the radio, and a voice message recorded from the
// radio's input would be the band, not the operator. So every sound card
// data2g-host records from or plays to is the radio's: the source at the
// other end of each data2g capture (as ear::findHostCapture finds it) and
// the sink at the other end of each data2g playback, the playback twin.
// All of them, not just the host Ragtime talks to: on the two-host bench
// both cards are radios. A card's other half goes too (its input and
// output share ear::GraphNode::device), and so do the cards Ragtime saw
// data2g-host use before, remembered by name, so the radio stays left out
// while data2g-host isn't running.
//
// Pure functions over ear::Graph, so tests build graphs by hand; PipeWire
// fills the graph on Linux and WASAPI's endpoints and sessions on Windows
// (audio::VoiceAudio).
#ifndef RAGTIME_VOICE_DEVICES_H
#define RAGTIME_VOICE_DEVICES_H

#include <string>
#include <vector>

#include "ragtime/ear/Graph.h"

namespace ragtime
{
namespace voice
{

// A radio card, and why it is one.
struct RadioCard
{
    ear::GraphNode node;
    bool input = false;         // a source; otherwise a sink
    // data2g-host records from it or plays to it now; otherwise it is
    // the other half of such a card, or remembered from before.
    bool inUse = false;
    bool remembered = false;
};

struct Devices
{
    std::vector<ear::GraphNode> microphones;    // sources, by description
    std::vector<ear::GraphNode> speakers;       // sinks, by description
    std::vector<RadioCard> radio;               // left out of both
    // The names data2g-host's cards go by now, sources and sinks, both
    // halves: to remember (rememberRadio).
    std::vector<std::string> radioNow;

    bool isRadio(const std::string& name) const;
};

// remembered: the radio cards' names from before (rememberRadio's).
Devices devices(const ear::Graph& graph, const std::vector<std::string>& remembered);

// remembered with radioNow added, in order, each once.
std::vector<std::string> rememberRadio(const std::vector<std::string>& remembered,
                                       const std::vector<std::string>& radioNow);

// Which device to use, and why. The operator's pick, if it is there and
// isn't the radio's; with no pick, the system's default, unless that is
// the radio's (a USB codec often becomes the default input when it is
// plugged in), when the first other device is used instead.
enum class Pick
{
    None,           // nothing to use: no devices, or only the radio's
    Chosen,
    Default,
    InsteadOfRadio, // the default is the radio's: another instead
    Missing,        // the pick isn't there (unplugged)
    Refused,        // the pick is the radio's
};

struct DevicePick
{
    ear::GraphNode node;        // valid unless why is None, Missing or Refused
    Pick why = Pick::None;
    bool usable() const { return why == Pick::Chosen || why == Pick::Default || why == Pick::InsteadOfRadio; }
};

// allowed: Devices::microphones or ::speakers; chosen: a node name, empty
// for the default; defaultName: the system default's name, empty when
// unknown.
DevicePick pickDevice(const std::vector<ear::GraphNode>& allowed, const Devices& all, const std::string& chosen,
                      const std::string& defaultName);

// What the pick means, for a status line: "Using USB Headset (the
// system's default)".
std::string describe(const DevicePick& pick, bool microphone);

// A device's name for people: its description, or its name.
std::string label(const ear::GraphNode& node);

// The remembered names as one wxConfig string and back, ';' between.
std::string joinNames(const std::vector<std::string>& names);
std::vector<std::string> splitNames(const std::string& text);

} // namespace voice
} // namespace ragtime

#endif
