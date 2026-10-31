#include "ragtime/ear/Graph.h"

#include <algorithm>

namespace ragtime
{
namespace ear
{

const GraphNode* Graph::node(uint32_t id) const
{
    for (const GraphNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const GraphClient* Graph::client(uint32_t id) const
{
    for (const GraphClient& c : clients)
        if (c.id == id) return &c;
    return nullptr;
}

bool isData2GProgram(const std::string& appName)
{
    return appName == "data2g-host" || appName == "data2g-gui";
}

bool isSource(const GraphNode& node)
{
    return node.mediaClass.rfind("Audio/Source", 0) == 0;
}

bool isSink(const GraphNode& node)
{
    return node.mediaClass.rfind("Audio/Sink", 0) == 0;
}

std::vector<GraphNode> sources(const Graph& graph)
{
    std::vector<GraphNode> out;
    for (const GraphNode& n : graph.nodes)
        if (isSource(n)) out.push_back(n);
    std::sort(out.begin(), out.end(), [](const GraphNode& a, const GraphNode& b) {
        const std::string& x = a.description.empty() ? a.name : a.description;
        const std::string& y = b.description.empty() ? b.name : b.description;
        return x != y ? x < y : a.id < b.id;
    });
    return out;
}

namespace
{

// The program a node belongs to: its client's name or its own, whichever
// is data2g's (a client's name comes from the binary, a node's from what
// the program calls itself, and either may say data2g-host).
std::string programOf(const Graph& graph, const GraphNode& node, int* pid)
{
    const GraphClient* c = graph.client(node.clientId);
    if (pid) *pid = c ? c->pid : 0;
    if (c && isData2GProgram(c->appName)) return c->appName;
    if (isData2GProgram(node.appName)) return node.appName;
    return c ? c->appName : node.appName;
}

const GraphNode* targetSource(const Graph& graph, const std::string& target)
{
    if (target.empty()) return nullptr;
    for (const GraphNode& n : graph.nodes)
        if (isSource(n) && (n.name == target || std::to_string(n.id) == target)) return &n;
    return nullptr;
}

} // namespace

HostSearch findHostCapture(const Graph& graph, std::optional<int> hostPid)
{
    HostSearch search;
    std::vector<HostCapture> captures;
    for (const GraphNode& n : graph.nodes)
    {
        if (n.mediaClass != "Stream/Input/Audio") continue;
        HostCapture c;
        c.program = programOf(graph, n, &c.pid);
        if (!isData2GProgram(c.program)) continue;
        c.stream = n.id;
        const GraphNode* source = nullptr;
        for (const GraphLink& l : graph.links)
        {
            if (l.inputNode != n.id) continue;
            const GraphNode* from = graph.node(l.outputNode);
            if (from && isSource(*from))
            {
                source = from;
                break;
            }
        }
        c.linked = source != nullptr;
        if (!source) source = targetSource(graph, n.target);
        if (source)
        {
            c.source = source->id;
            c.sourceName = source->name;
            c.sourceDescription = source->description.empty() ? source->name : source->description;
        }
        captures.push_back(c);
    }
    search.candidates = (int)captures.size();

    if (captures.empty())
    {
        search.why = "data2g-host isn't recording from a sound card";
        return search;
    }
    if (hostPid)
    {
        for (const HostCapture& c : captures)
            if (c.pid == *hostPid)
            {
                search.found = c;
                break;
            }
    }
    if (!search.found)
    {
        if (captures.size() == 1)
            search.found = captures.front();
        else
        {
            search.why = "several data2g-hosts are recording, and none is the one Ragtime talks to";
            return search;
        }
    }
    if (search.found->source == 0) search.why = "data2g-host's recording isn't linked to a sound card yet";
    return search;
}

namespace
{

const GraphNode* sourceNamed(const Graph& graph, const std::string& name)
{
    if (name.empty()) return nullptr;
    for (const GraphNode& n : graph.nodes)
        if (isSource(n) && n.name == name) return &n;
    return nullptr;
}

} // namespace

SourcePick pickSource(const Graph& graph, const std::string& chosen, const HostSearch& host,
                      const std::string& lastHostSource, const std::string& defaultSource)
{
    SourcePick pick;
    const uint32_t hostSource = host.found ? host.found->source : 0;
    if (!chosen.empty())
    {
        // A pick that has gone is not swapped for another: the dropdown
        // says it's missing.
        pick.node = sourceNamed(graph, chosen);
        pick.why = pick.node ? Pick::Chosen : Pick::None;
    }
    else if (hostSource != 0 && graph.node(hostSource))
    {
        pick.node = graph.node(hostSource);
        pick.why = Pick::Host;
    }
    else if ((pick.node = sourceNamed(graph, lastHostSource)) != nullptr)
    {
        pick.why = Pick::HostBefore;
    }
    else if ((pick.node = sourceNamed(graph, defaultSource)) != nullptr)
    {
        pick.why = Pick::Default;
    }
    pick.passive = pick.node && hostSource != 0 && pick.node->id == hostSource;
    return pick;
}

} // namespace ear
} // namespace ragtime
