#include "ragtime/ear/ProcNet.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace ragtime
{
namespace ear
{

namespace fs = std::filesystem;

std::vector<unsigned long> listeningInodes(const std::string& procNetTcp, int port)
{
    // "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode"
    // "   0: 0100007F:206C 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 123456 ..."
    std::vector<unsigned long> inodes;
    std::istringstream in(procNetTcp);
    std::string line;
    std::getline(in, line);   // the heading
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string slot, local, remote, state, queues, timer, retransmits, uid, timeout, inode;
        if (!(fields >> slot >> local >> remote >> state >> queues >> timer >> retransmits >> uid >> timeout >> inode))
            continue;
        if (state != "0A") continue;
        const std::size_t colon = local.rfind(':');
        if (colon == std::string::npos) continue;
        if (std::strtol(local.c_str() + colon + 1, nullptr, 16) != port) continue;
        const unsigned long number = std::strtoul(inode.c_str(), nullptr, 10);
        if (number != 0) inodes.push_back(number);
    }
    return inodes;
}

std::optional<int> processListeningOn(int port, const std::string& procRoot)
{
    std::vector<unsigned long> inodes;
    for (const char* table : {"net/tcp", "net/tcp6"})
    {
        std::ifstream f(fs::path(procRoot) / table);
        if (!f) continue;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        for (unsigned long i : listeningInodes(text, port)) inodes.push_back(i);
    }
    if (inodes.empty()) return std::nullopt;

    std::vector<std::string> wanted;
    for (unsigned long i : inodes) wanted.push_back("socket:[" + std::to_string(i) + "]");

    std::error_code ec;
    for (const fs::directory_entry& process : fs::directory_iterator(procRoot, ec))
    {
        const std::string pid = process.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        std::error_code fdError;
        for (const fs::directory_entry& fd : fs::directory_iterator(process.path() / "fd", fdError))
        {
            std::error_code linkError;
            const fs::path target = fs::read_symlink(fd.path(), linkError);
            if (linkError) continue;
            if (std::find(wanted.begin(), wanted.end(), target.string()) != wanted.end()) return std::atoi(pid.c_str());
        }
    }
    return std::nullopt;
}

} // namespace ear
} // namespace ragtime
