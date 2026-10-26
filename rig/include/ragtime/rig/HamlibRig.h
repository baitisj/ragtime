// HamlibRig: Ragtime opening the rig itself with Hamlib (Settings > Rig,
// "Hamlib"), on a thread of its own, so a slow serial port never holds up
// the window.
//
// It opens the rig with the settings' configuration words
// (radio::hamlibConf), reads the dial every pollSeconds, and sets PTT when
// told to, putting a PTT change ahead of any reading and never reading
// while keyed. A rig that fails to open, or stops answering, is closed and
// tried again 2, 4, 8, 15 and then every 30 s (link::Reconnect). Stopping
// unkeys the rig, if keyed, before closing it.
//
// It never tunes the rig or sets its mode: the radio is the source of
// truth for frequency. PTT is the only thing it sets, and only when told
// to (PttFollower, with "Key the rig when data2g-host keys" ticked).
//
// Built without Hamlib (-DRAGTIME_HAMLIB=OFF), available() is false and a
// HamlibRig only says so.
#ifndef RAGTIME_RIG_HAMLIB_RIG_H
#define RAGTIME_RIG_HAMLIB_RIG_H

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ragtime/radio/RigSettings.h"

namespace ragtime
{
namespace rig
{

// Built with Hamlib.
bool available();
// "Hamlib 4.5.5", or why there is none.
std::string hamlibVersion();

// A rig Hamlib knows.
struct Model
{
    int id = 0;
    std::string maker;      // "Icom"
    std::string name;       // "IC-7100"
    std::string status;     // "Stable", "Beta", "Alpha", "Untested"
    radio::PortType port = radio::PortType::Serial;
};

// Every rig Hamlib knows, by maker and name, but NET rigctl: reading the
// dial from rigctld is the Rig page's own choice, and keying through a
// shared rigctld is what Ragtime never does.
std::vector<Model> models();
// One of them; nullopt if Hamlib doesn't know it.
std::optional<Model> model(int id);

// A backend's own settings, beyond the port's: civaddr for an Icom.
struct BackendParam
{
    std::string name;       // "civaddr"
    std::string label;      // "Transceiver's CI-V address"
    std::string fallback;   // its default, as Hamlib writes it
};
std::vector<BackendParam> backendParams(int model);

// The serial ports this computer has, for the port box: /dev/serial/by-id
// names first, then /dev/ttyUSB* and /dev/ttyACM*; COM ports on Windows.
std::vector<std::string> serialPorts();

class HamlibRig
{
public:
    struct Status
    {
        bool running = false;           // started, not stopped
        bool open = false;              // the rig answered
        bool keyed = false;             // PTT is on, as last set
        std::optional<uint64_t> hz;     // the dial, while open
        std::string rig;                // "Icom IC-7100"
        // Why the rig isn't open or the last command failed, and when the
        // next try is: "Communication timed out; trying again in 8 s".
        std::string problem;
        // Goes up each time setting PTT fails, or the rig closes while
        // keyed: the follower unkeys.
        uint64_t pttFailures = 0;
        uint64_t version = 0;           // goes up with any change
    };

    HamlibRig();
    ~HamlibRig();
    HamlibRig(const HamlibRig&) = delete;
    HamlibRig& operator=(const HamlibRig&) = delete;

    // Opens the rig with these settings (closing any open one first), and
    // keeps it open until stop().
    void start(const radio::RigSettings& settings);
    // Unkeys if keyed, then closes the rig. Waits for the thread.
    void stop();
    // Reads the dial this often, from now.
    void setPollSeconds(int seconds);

    // Keys or unkeys the rig, as soon as the thread gets to it.
    void setPtt(bool on);

    Status status() const;

private:
    struct Shared;
    void run();

    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

} // namespace rig
} // namespace ragtime

#endif
