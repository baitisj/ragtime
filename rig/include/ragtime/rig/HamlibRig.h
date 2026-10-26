// HamlibRig: Ragtime opening the rig itself with Hamlib (Settings > Rig,
// "Hamlib"), on a thread of its own, so a slow serial port never holds up
// the window.
//
// It opens the rig with the settings' configuration words
// (radio::hamlibConf), reads the dial every pollSeconds, and sets PTT when
// told to, putting a PTT change ahead of any reading. While the rig
// transmits (data2g-host's PTT ON, whoever keys it) it reads the transmit
// meters the rig reports, ALC, SWR and RF power, one every METER_MS, in
// place of the dial. With the SWR trip on (radio::SwrTrip, Settings >
// Rig), SWR is every other reading, and on a trip the thread itself
// unkeys the rig, whoever keyed it, sets its power (RFPOWER) to 0, and
// keys it no more until clearSwrTrip(). The power is never put back.
// Between bursts it reads the S-meter (STRENGTH, dB over S9) every
// radio::NoiseFloor::READ_MS, but not in the first SETTLE_MS after one, for
// the band's noise. While the Ear is open (setFilterWatch) it also reads
// the receive filter between bursts every FILTER_MS: the mode with its
// passband width, and PBT_IN and PBT_OUT where the rig has them
// (radio::RigFilter). A rig that fails to open, or stops answering, is closed and
// tried again 2, 4, 8, 15 and then every 30 s (link::Reconnect). Stopping
// unkeys the rig, if keyed, before closing it.
//
// It never tunes the rig or sets its mode: the radio is the source of
// truth for frequency. PTT is the only thing it sets when told to
// (PttFollower, with "Key the rig when data2g-host keys" ticked), and the
// SWR trip the only thing that sets the power.
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

#include "ragtime/radio/RigFilter.h"
#include "ragtime/radio/RigSettings.h"
#include "ragtime/radio/TxMeters.h"

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
        uint64_t version = 0;           // goes up with any change but a meter

        // The transmit meters this rig reports, once open.
        bool readsAlc = false;
        bool readsSwr = false;
        bool readsPower = false;
        // The newest of each, while transmitting; empty between bursts.
        radio::MeterReading meters;
        // Each goes up as its meter is read.
        uint64_t alcReadings = 0;
        uint64_t swrReadings = 0;
        uint64_t powerReadings = 0;
        uint64_t meterReadings = 0;     // any of them

        // The S-meter, in dB over S9, read between bursts.
        bool readsStrength = false;
        std::optional<double> strength;
        uint64_t strengthReadings = 0;  // goes up as it is read

        // The receive filter, while the Ear watches it.
        std::optional<radio::RigFilter> filter;

        // The SWR trip, from the trip until clearSwrTrip().
        bool swrTripped = false;
        // What was done: "SWR 3.6: Ragtime unkeyed the rig and set its
        // power to 0 (it was 50%)".
        std::string tripText;
        uint64_t swrTrips = 0;          // goes up with each trip
    };

    // One meter is read this often while transmitting.
    static constexpr int METER_MS = 200;
    // While the Ear is open, the filter is read this often between bursts.
    static constexpr int FILTER_MS = 2000;

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
    // The rig is transmitting, keyed by us or not: read its meters, not
    // its dial.
    void setTransmitting(bool on);
    // The Ear is open: read the filter between bursts. Off forgets it.
    void setFilterWatch(bool on);
    // Settings > Rig's SWR trip, on or off, from now.
    void setSwrTrip(bool on);
    // The operator cleared a trip: the rig may be keyed again. Its power
    // stays where the trip put it.
    void clearSwrTrip();

    // For tests on Hamlib's dummy rig: sets a level ("SWR", "RFPOWER")
    // straight in the backend, read-only meters included.
    void setDummyLevelForTest(const std::string& level, double value);

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
