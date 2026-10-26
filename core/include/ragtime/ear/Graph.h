// Graph: finding the sound card data2g-host listens to, from what PipeWire
// says about its graph, so the Ear can listen to the same one alongside it
// (docs/STAGE1.md, "The Ear").
//
// PipeWire tells every program the graph's nodes (sound cards' sources and
// sinks, and each program's streams), the clients that own them (with the
// process id the kernel vouches for, pipewire.sec.pid) and the links
// between them. data2g-host's capture is a "Stream/Input/Audio" node whose
// client is named data2g-host, or data2g-gui when the host runs inside
// Data2G's window (as on Jeff's radio PC). Its source is the node at the
// other end of the link feeding it: the stream's target (target.object,
// node.target) is only what it asked for, so it counts only while no link
// is up.
//
// With several hosts running (the two-host bench), the one whose process
// listens on the command port Ragtime talks to wins; ProcNet.h finds it.
//
// Pure functions over plain structs, so tests build graphs by hand.
#ifndef RAGTIME_EAR_GRAPH_H
#define RAGTIME_EAR_GRAPH_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ragtime
{
namespace ear
{

struct GraphNode
{
    uint32_t id = 0;
    uint32_t clientId = 0;
    std::string name;           // node.name
    std::string description;    // node.description, or node.nick
    std::string mediaClass;     // media.class
    std::string appName;        // application.name, when the node has it
    std::string target;         // target.object, or node.target
};

struct GraphClient
{
    uint32_t id = 0;
    std::string appName;        // application.name
    int pid = 0;                // pipewire.sec.pid
};

struct GraphLink
{
    uint32_t outputNode = 0;
    uint32_t inputNode = 0;
};

struct Graph
{
    std::vector<GraphNode> nodes;
    std::vector<GraphClient> clients;
    std::vector<GraphLink> links;

    const GraphNode* node(uint32_t id) const;
    const GraphClient* client(uint32_t id) const;
};

// The programs whose capture is data2g-host's.
bool isData2GProgram(const std::string& appName);
// A sound card's input (or a virtual one), not a program's stream.
bool isSource(const GraphNode& node);

// The sources the Ear's dropdown offers, by description.
std::vector<GraphNode> sources(const Graph& graph);

struct HostCapture
{
    uint32_t stream = 0;        // data2g-host's capture node
    std::string program;        // data2g-host or data2g-gui
    int pid = 0;
    // The source it listens to: 0 and empty while unknown (no link yet,
    // and a target that names no source).
    uint32_t source = 0;
    std::string sourceName;
    std::string sourceDescription;
    bool linked = false;        // found by its link, not just its target
};

struct HostSearch
{
    std::optional<HostCapture> found;
    int candidates = 0;         // data2g captures seen
    // Why nothing was found, in words for the Ear's status line.
    std::string why;
};

// data2g-host's capture, and the source it listens to. hostPid, when
// known, is the process listening on the host's command port.
HostSearch findHostCapture(const Graph& graph, std::optional<int> hostPid);

} // namespace ear
} // namespace ragtime

#endif
