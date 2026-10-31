#include "ragtime/voice/Devices.h"

#include <algorithm>
#include <cstddef>
#include <set>

namespace ragtime
{
namespace voice
{

namespace
{

bool isData2GNode(const ear::Graph& graph, const ear::GraphNode& node)
{
    const ear::GraphClient* c = graph.client(node.clientId);
    return (c && ear::isData2GProgram(c->appName)) || ear::isData2GProgram(node.appName);
}

const ear::GraphNode* targetNode(const ear::Graph& graph, const std::string& target, bool source)
{
    if (target.empty()) return nullptr;
    for (const ear::GraphNode& n : graph.nodes)
        if ((source ? ear::isSource(n) : ear::isSink(n)) && (n.name == target || std::to_string(n.id) == target))
            return &n;
    return nullptr;
}

bool contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

void sortByLabel(std::vector<ear::GraphNode>& nodes)
{
    std::sort(nodes.begin(), nodes.end(), [](const ear::GraphNode& a, const ear::GraphNode& b) {
        const std::string x = label(a);
        const std::string y = label(b);
        return x != y ? x < y : a.id < b.id;
    });
}

} // namespace

bool Devices::isRadio(const std::string& name) const
{
    if (name.empty()) return false;
    if (contains(radioNow, name)) return true;
    for (const RadioCard& r : radio)
        if (r.node.name == name) return true;
    return false;
}

std::string label(const ear::GraphNode& node)
{
    return node.description.empty() ? node.name : node.description;
}

Devices devices(const ear::Graph& graph, const std::vector<std::string>& remembered)
{
    // The nodes data2g's streams are linked to, or ask for while no link
    // is up.
    std::set<uint32_t> used;
    for (const ear::GraphNode& stream : graph.nodes)
    {
        const bool capture = stream.mediaClass == "Stream/Input/Audio";
        const bool playback = stream.mediaClass == "Stream/Output/Audio";
        if ((!capture && !playback) || !isData2GNode(graph, stream)) continue;
        bool linked = false;
        for (const ear::GraphLink& l : graph.links)
        {
            const ear::GraphNode* other = graph.node(capture ? l.outputNode : l.inputNode);
            if ((capture ? l.inputNode : l.outputNode) != stream.id || !other) continue;
            if (capture ? ear::isSource(*other) : ear::isSink(*other))
            {
                used.insert(other->id);
                linked = true;
            }
        }
        if (!linked)
            if (const ear::GraphNode* t = targetNode(graph, stream.target, capture)) used.insert(t->id);
    }
    // Their cards, both halves.
    std::set<std::string> cards;
    for (uint32_t id : used)
        if (const ear::GraphNode* n = graph.node(id))
            if (!n->device.empty()) cards.insert(n->device);

    Devices out;
    for (const ear::GraphNode& n : graph.nodes)
    {
        const bool source = ear::isSource(n);
        if (!source && !ear::isSink(n)) continue;
        RadioCard r;
        r.node = n;
        r.input = source;
        r.inUse = used.count(n.id) != 0;
        const bool twin = !n.device.empty() && cards.count(n.device) != 0;
        r.remembered = contains(remembered, n.name);
        if (r.inUse || twin)
        {
            if (!contains(out.radioNow, n.name)) out.radioNow.push_back(n.name);
        }
        if (r.inUse || twin || r.remembered)
            out.radio.push_back(r);
        else
            (source ? out.microphones : out.speakers).push_back(n);
    }
    sortByLabel(out.microphones);
    sortByLabel(out.speakers);
    std::sort(out.radio.begin(), out.radio.end(), [](const RadioCard& a, const RadioCard& b) {
        if (a.input != b.input) return a.input;
        return label(a.node) < label(b.node);
    });
    return out;
}

std::vector<std::string> rememberRadio(const std::vector<std::string>& remembered,
                                       const std::vector<std::string>& radioNow)
{
    std::vector<std::string> out;
    for (const std::string& n : remembered)
        if (!n.empty() && !contains(out, n)) out.push_back(n);
    for (const std::string& n : radioNow)
        if (!n.empty() && !contains(out, n)) out.push_back(n);
    return out;
}

DevicePick pickDevice(const std::vector<ear::GraphNode>& allowed, const Devices& all, const std::string& chosen,
                      const std::string& defaultName)
{
    DevicePick pick;
    auto find = [&](const std::string& name) -> const ear::GraphNode* {
        for (const ear::GraphNode& n : allowed)
            if (n.name == name) return &n;
        return nullptr;
    };
    if (!chosen.empty())
    {
        // A pick that has gone is not swapped for another, as the Ear's
        // isn't: the status line says it's missing.
        if (all.isRadio(chosen))
            pick.why = Pick::Refused;
        else if (const ear::GraphNode* n = find(chosen))
        {
            pick.node = *n;
            pick.why = Pick::Chosen;
        }
        else
            pick.why = Pick::Missing;
        return pick;
    }
    if (const ear::GraphNode* n = find(defaultName))
    {
        pick.node = *n;
        pick.why = Pick::Default;
        return pick;
    }
    // No default known, or it's the radio's (or gone): the first other.
    if (!allowed.empty())
    {
        pick.node = allowed.front();
        pick.why = all.isRadio(defaultName) ? Pick::InsteadOfRadio : Pick::Default;
    }
    return pick;
}

std::string describe(const DevicePick& pick, bool microphone)
{
    const std::string what = microphone ? "microphone" : "speaker";
    switch (pick.why)
    {
    case Pick::Chosen:
        return "Using " + label(pick.node);
    case Pick::Default:
        return "Using " + label(pick.node) + ", the system's " + (microphone ? "input" : "output");
    case Pick::InsteadOfRadio:
        return "Using " + label(pick.node) + ": the system's " + (microphone ? "input" : "output") +
               " is the radio's";
    case Pick::Missing:
        return "The " + what + " picked isn't there: plug it in, or pick another";
    case Pick::Refused:
        return "That is the radio's sound card: pick your own " + what;
    case Pick::None:
        break;
    }
    return microphone ? "No microphone found, other than the radio's" : "No speaker found, other than the radio's";
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
    {
        if (n.empty()) continue;
        if (!out.empty()) out += ';';
        out += n;
    }
    return out;
}

std::vector<std::string> splitNames(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

} // namespace voice
} // namespace ragtime
