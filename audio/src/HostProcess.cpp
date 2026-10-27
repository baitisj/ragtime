// audio::processListeningOn (EarAudio.h): /proc on Linux, Windows' TCP
// table there.
#include "ragtime/audio/EarAudio.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include "ragtime/ear/ProcNet.h"
#endif

#include <vector>

namespace ragtime
{
namespace audio
{

#ifdef _WIN32

namespace
{

// A table's local port is in network order in its low 16 bits.
int portOf(DWORD local)
{
    return (int)(((local & 0xff) << 8) | ((local >> 8) & 0xff));
}

template <typename Table>
std::optional<int> owner(ULONG family, int port)
{
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return std::nullopt;
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return std::nullopt;
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; i++)
        if (portOf(table->table[i].dwLocalPort) == port) return (int)table->table[i].dwOwningPid;
    return std::nullopt;
}

} // namespace

std::optional<int> processListeningOn(int port)
{
    if (auto pid = owner<MIB_TCPTABLE_OWNER_PID>(AF_INET, port)) return pid;
    return owner<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, port);
}

#else

std::optional<int> processListeningOn(int port)
{
    return ear::processListeningOn(port);
}

#endif

} // namespace audio
} // namespace ragtime
