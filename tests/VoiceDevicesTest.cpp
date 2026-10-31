// Voice's microphones and speakers: the radio's card is left out, both
// halves of it, whichever data2g program uses it, by link or by target;
// cards seen before stay left out while data2g-host is stopped; and the
// pick falls past a default that is the radio's, refuses the radio picked
// by hand and never swaps a missing pick for another.
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/voice/Devices.h"

using namespace ragtime;
using ragtime::ear::Graph;
using ragtime::ear::GraphNode;

namespace
{

const std::string CODEC_IN = "alsa_input.usb-Burr-Brown_USB_Audio_CODEC-00.analog-stereo-input";
const std::string CODEC_OUT = "alsa_output.usb-Burr-Brown_USB_Audio_CODEC-00.analog-stereo-output";
const std::string BUILT_IN_IN = "alsa_input.pci-0000_00_1f.3.analog-stereo";
const std::string BUILT_IN_OUT = "alsa_output.pci-0000_00_1f.3.analog-stereo";
const std::string HEADSET_IN = "alsa_input.usb-Logitech_USB_Headset-00.mono-fallback";
const std::string HEADSET_OUT = "alsa_output.usb-Logitech_USB_Headset-00.analog-stereo";

GraphNode node(uint32_t id, const std::string& name, const std::string& description, const std::string& mediaClass,
               const std::string& device)
{
    GraphNode n;
    n.id = id;
    n.name = name;
    n.description = description;
    n.mediaClass = mediaClass;
    n.device = device;
    return n;
}

// Jeff's radio PC, more or less: the radio's codec, the built-in card and
// a headset, and data2g-gui recording from the codec and playing to it.
Graph station()
{
    Graph g;
    g.nodes = {
        node(55, CODEC_IN, "USB Audio CODEC Analog Stereo", "Audio/Source", "41"),
        node(56, CODEC_OUT, "USB Audio CODEC Analog Stereo", "Audio/Sink", "41"),
        node(60, BUILT_IN_IN, "Built-in Audio Analog Stereo", "Audio/Source", "42"),
        node(61, BUILT_IN_OUT, "Built-in Audio Analog Stereo", "Audio/Sink", "42"),
        node(70, HEADSET_IN, "Logitech USB Headset Mono", "Audio/Source", "43"),
        node(71, HEADSET_OUT, "Logitech USB Headset Analog Stereo", "Audio/Sink", "43"),
    };
    GraphNode capture = node(129, "data2g-gui", "", "Stream/Input/Audio", "");
    capture.clientId = 54;
    GraphNode playback = node(130, "data2g-gui", "", "Stream/Output/Audio", "");
    playback.clientId = 54;
    g.nodes.push_back(capture);
    g.nodes.push_back(playback);
    ear::GraphClient gui;
    gui.id = 54;
    gui.appName = "data2g-gui";
    gui.pid = 205757;
    g.clients = {gui};
    g.links = {{55, 129}, {130, 56}};
    return g;
}

bool has(const std::vector<GraphNode>& nodes, const std::string& name)
{
    for (const GraphNode& n : nodes)
        if (n.name == name) return true;
    return false;
}

void leavesOutTheRadio()
{
    voice::Devices d = voice::devices(station(), {});
    CHECK_EQ(d.microphones.size(), 2u);
    CHECK_EQ(d.speakers.size(), 2u);
    CHECK(!has(d.microphones, CODEC_IN));
    CHECK(!has(d.speakers, CODEC_OUT));
    CHECK(has(d.microphones, HEADSET_IN));
    CHECK(has(d.speakers, BUILT_IN_OUT));
    // By description: Built-in before Logitech.
    if (d.microphones.size() == 2) CHECK_EQ(d.microphones[0].name, BUILT_IN_IN);
    CHECK_EQ(d.radio.size(), 2u);
    if (d.radio.size() == 2)
    {
        CHECK(d.radio[0].input && d.radio[0].inUse && d.radio[0].node.name == CODEC_IN);
        CHECK(!d.radio[1].input && d.radio[1].inUse && d.radio[1].node.name == CODEC_OUT);
    }
    CHECK(d.isRadio(CODEC_IN) && d.isRadio(CODEC_OUT) && !d.isRadio(HEADSET_IN) && !d.isRadio(""));
    CHECK_EQ(d.radioNow.size(), 2u);
    // Streams are never offered.
    CHECK(!has(d.microphones, "data2g-gui") && !has(d.speakers, "data2g-gui"));
}

void bothHalves()
{
    // data2g-host only records (receive only, or its playback not up yet):
    // the codec's output is still the radio's, by its card.
    Graph g = station();
    g.links = {{55, 129}};
    g.nodes.erase(g.nodes.begin() + 7);
    voice::Devices d = voice::devices(g, {});
    CHECK(!has(d.speakers, CODEC_OUT));
    CHECK(d.isRadio(CODEC_OUT));
    CHECK_EQ(d.radio.size(), 2u);
    if (d.radio.size() == 2) CHECK(!d.radio[1].inUse);
    CHECK_EQ(d.radioNow.size(), 2u);

    // On Windows built-in devices have no card id: their other half stays.
    g = station();
    for (GraphNode& n : g.nodes) n.device.clear();
    g.links = {{130, 61}};
    d = voice::devices(g, {});
    CHECK(!has(d.speakers, BUILT_IN_OUT));
    CHECK(has(d.microphones, BUILT_IN_IN));
}

void byTargetAndProgram()
{
    // No links yet: the streams' targets stand in.
    Graph g = station();
    g.links.clear();
    g.nodes[6].target = CODEC_IN;
    g.nodes[7].target = "56";
    for (GraphNode& n : g.nodes) n.device.clear();
    voice::Devices d = voice::devices(g, {});
    CHECK(!has(d.microphones, CODEC_IN));
    CHECK(!has(d.speakers, CODEC_OUT));

    // A program that isn't data2g's makes nothing the radio's.
    g = station();
    g.clients[0].appName = "fldigi";
    d = voice::devices(g, {});
    CHECK_EQ(d.microphones.size(), 3u);
    CHECK(d.radio.empty());

    // The node's own name counts, as for the Ear.
    g.nodes[6].appName = "data2g-host";
    d = voice::devices(g, {});
    CHECK(!has(d.microphones, CODEC_IN));
}

void twoHosts()
{
    // The two-host bench: both cards are radios, whichever host Ragtime
    // talks to.
    Graph g = station();
    GraphNode capture = node(140, "data2g-host", "", "Stream/Input/Audio", "");
    capture.clientId = 90;
    g.nodes.push_back(capture);
    ear::GraphClient host;
    host.id = 90;
    host.appName = "data2g-host";
    g.clients.push_back(host);
    g.links.push_back({70, 140});
    voice::Devices d = voice::devices(g, {});
    CHECK_EQ(d.microphones.size(), 1u);
    CHECK_EQ(d.speakers.size(), 1u);
    CHECK(has(d.microphones, BUILT_IN_IN));
}

void remembered()
{
    // data2g-host stopped: nothing in the graph says which card is the
    // radio, but the names seen before do.
    Graph g = station();
    std::vector<std::string> seen = voice::devices(g, {}).radioNow;
    g.nodes.resize(6);
    g.links.clear();
    voice::Devices fresh = voice::devices(g, {});
    CHECK_EQ(fresh.microphones.size(), 3u);
    voice::Devices d = voice::devices(g, seen);
    CHECK_EQ(d.microphones.size(), 2u);
    CHECK_EQ(d.speakers.size(), 2u);
    CHECK_EQ(d.radio.size(), 2u);
    if (!d.radio.empty()) CHECK(d.radio[0].remembered && !d.radio[0].inUse);
    CHECK(d.radioNow.empty());

    std::vector<std::string> both = voice::rememberRadio({CODEC_IN, ""}, {CODEC_IN, CODEC_OUT, HEADSET_IN});
    CHECK_EQ(both.size(), 3u);
    CHECK_EQ(voice::joinNames(both), CODEC_IN + ";" + CODEC_OUT + ";" + HEADSET_IN);
    CHECK(voice::splitNames(voice::joinNames(both)) == both);
    CHECK(voice::splitNames("").empty());
    CHECK(voice::splitNames(";a;;b;") == std::vector<std::string>({"a", "b"}));
}

void picks()
{
    voice::Devices d = voice::devices(station(), {});
    // The default, when it isn't the radio's.
    voice::DevicePick p = voice::pickDevice(d.microphones, d, "", HEADSET_IN);
    CHECK(p.why == voice::Pick::Default && p.node.name == HEADSET_IN && p.usable());
    // The default is the radio's codec (plugging it in can do that): the
    // first other instead.
    p = voice::pickDevice(d.microphones, d, "", CODEC_IN);
    CHECK(p.why == voice::Pick::InsteadOfRadio && p.node.name == BUILT_IN_IN && p.usable());
    CHECK(voice::describe(p, true).find("radio") != std::string::npos);
    p = voice::pickDevice(d.speakers, d, "", CODEC_OUT);
    CHECK(p.why == voice::Pick::InsteadOfRadio && p.node.name == BUILT_IN_OUT);
    // No default known: the first.
    p = voice::pickDevice(d.speakers, d, "", "");
    CHECK(p.why == voice::Pick::Default && p.node.name == BUILT_IN_OUT);
    // Picked by hand.
    p = voice::pickDevice(d.speakers, d, HEADSET_OUT, BUILT_IN_OUT);
    CHECK(p.why == voice::Pick::Chosen && p.node.name == HEADSET_OUT);
    // The radio picked by hand (saved before data2g-host used it): refused.
    p = voice::pickDevice(d.microphones, d, CODEC_IN, BUILT_IN_IN);
    CHECK(p.why == voice::Pick::Refused && !p.usable());
    // Unplugged: missing, not swapped for another.
    p = voice::pickDevice(d.microphones, d, "alsa_input.gone", BUILT_IN_IN);
    CHECK(p.why == voice::Pick::Missing && !p.usable());
    // Only the radio: nothing.
    voice::Devices onlyRadio = d;
    onlyRadio.microphones.clear();
    p = voice::pickDevice(onlyRadio.microphones, onlyRadio, "", CODEC_IN);
    CHECK(p.why == voice::Pick::None && !p.usable());
    CHECK(!voice::describe(p, true).empty());
}

} // namespace

int main()
{
    leavesOutTheRadio();
    bothHalves();
    byTargetAndProgram();
    twoHosts();
    remembered();
    picks();
    return ragtime_test::finish();
}
