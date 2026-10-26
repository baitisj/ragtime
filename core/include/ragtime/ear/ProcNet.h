// ProcNet: which process listens on a TCP port on this machine, read from
// Linux's /proc, so the Ear can tell which of several data2g-hosts is the
// one Ragtime talks to (Graph.h). Only a host on this machine can be
// found; anywhere else the answer is "none".
//
// /proc/net/tcp and tcp6 list each socket with its local address and port
// in hex, its state (0A is LISTEN) and its inode; /proc/<pid>/fd/<n> links
// to "socket:[<inode>]" for each socket a process holds. The root is a
// parameter so tests can build a pretend /proc.
#ifndef RAGTIME_EAR_PROC_NET_H
#define RAGTIME_EAR_PROC_NET_H

#include <optional>
#include <string>
#include <vector>

namespace ragtime
{
namespace ear
{

// The inodes of the sockets listening on port, from the text of
// /proc/net/tcp or /proc/net/tcp6.
std::vector<unsigned long> listeningInodes(const std::string& procNetTcp, int port);

// The process listening on port, if one on this machine is and we may see
// its file descriptors.
std::optional<int> processListeningOn(int port, const std::string& procRoot = "/proc");

} // namespace ear
} // namespace ragtime

#endif
