#include <algorithm>
#include <string>
#include <vector>

#include "ragtime/rig/HamlibRig.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace ragtime
{
namespace rig
{

#ifdef _WIN32

std::vector<std::string> serialPorts()
{
    // Every DOS device name; the COM ports among them.
    std::vector<char> names(65536);
    DWORD length = QueryDosDeviceA(nullptr, names.data(), (DWORD)names.size());
    std::vector<std::string> ports;
    for (DWORD i = 0; i < length && names[i] != '\0';)
    {
        std::string name(&names[i]);
        i += (DWORD)name.size() + 1;
        if (name.size() > 3 && name.compare(0, 3, "COM") == 0
            && name.find_first_not_of("0123456789", 3) == std::string::npos)
            ports.push_back(name);
    }
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return std::stoi(a.substr(3)) < std::stoi(b.substr(3));
    });
    return ports;
}

#else

namespace
{

// The names in a folder starting with any of these, sorted, as paths.
std::vector<std::string> listed(const std::string& folder, const std::vector<std::string>& starts)
{
    std::vector<std::string> found;
    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) return found;
    while (struct dirent* entry = readdir(dir))
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        bool wanted = starts.empty();
        for (const std::string& start : starts) wanted = wanted || name.compare(0, start.size(), start) == 0;
        if (wanted) found.push_back(folder + "/" + name);
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

std::vector<std::string> serialPorts()
{
    // By-id names first: they survive replugging, and name the interface.
    std::vector<std::string> ports = listed("/dev/serial/by-id", {});
    for (const std::string& port : listed("/dev", {"ttyUSB", "ttyACM"})) ports.push_back(port);
    return ports;
}

#endif

} // namespace rig
} // namespace ragtime
