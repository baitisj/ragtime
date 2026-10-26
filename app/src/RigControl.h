#ifndef RAGTIME_RIG_CONTROL_H
#define RAGTIME_RIG_CONTROL_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <wx/event.h>
#include <wx/timer.h>

#include "ragtime/radio/Noise.h"
#include "ragtime/radio/PttFollower.h"
#include "ragtime/radio/RigFilter.h"
#include "ragtime/radio/RigSettings.h"
#include "ragtime/radio/TxMeters.h"
#include "ragtime/rig/HamlibRig.h"

class RigConnection;

// The radio as Settings > Rig has it: nothing, the dial read from rigctld
// (RigConnection, read only), or the rig opened with Hamlib
// (rig::HamlibRig), which follows data2g-host's PTT when "Key the rig when
// data2g-host keys" is ticked (radio::PttFollower: off by default, and
// unkeyed if the host link drops, the rig fails, Ragtime closes, or after
// 180 s keyed). The window asks it the dial, whichever way it is read,
// the transmit meters (radio::TxMeters) read while data2g-host transmits,
// and the band's noise from the S-meter read between bursts
// (radio::NoiseFloor), for the gearshift.
class RigControl : public wxEvtHandler
{
public:
    // changed: the reading, the connection or the keying changed.
    // meters: a transmit meter reading came in, or a burst began or ended.
    // tripped: the SWR trip tripped, with what the rig's thread did.
    RigControl(std::function<void()> changed, std::function<void()> meters,
               std::function<void(const std::string&)> tripped);
    ~RigControl() override;

    // Starts, restarts or stops what the settings name; a rig already open
    // the same way stays open.
    void apply(const ragtime::radio::RigSettings& settings);
    // Unkeys and closes, for quitting.
    void stop();
    // A line for the flight recorder: rigctld connected, failed, or
    // answered with an error.
    void setNote(std::function<void(const std::string&)> note);

    // data2g-host's PTT ON or OFF.
    void hostPtt(bool on);
    // The command link to data2g-host dropped.
    void hostLost();

    std::optional<uint64_t> dialHz() const;
    // "Hamlib, Icom IC-7100 on /dev/ttyUSB0: reading the dial every 1 s",
    // and why not, for the dial's tooltip.
    std::string describe() const;
    // Ragtime is keying the rig now.
    bool keying() const { return follower_.keyed(); }

    // The transmit meters, burst by burst; empty with no Hamlib rig.
    const ragtime::radio::TxMeters& meters() const { return meters_; }
    // What the open Hamlib rig reports: ALC, SWR, RF power.
    bool readsAlc() const;
    bool readsSwr() const;
    bool readsMeters() const;
    // The rig reports its S-meter, and the band's noise from it: the
    // quietest reading of the last minute on this band, in dB over S9.
    bool readsNoise() const;
    std::optional<double> noiseDb() const;
    // The Ear is open: read the rig's receive filter between bursts, its
    // mode, width and PBT (radio::RigFilter), through rigctld or Hamlib.
    // Read only.
    void setFilterWatch(bool on);
    std::optional<ragtime::radio::RigFilter> filter() const;
    // The Ear may set the filter: Hamlib, with "Let the Ear change the
    // rig's filters" ticked, and the rig open.
    bool canSetFilter() const;
    // Writes it (Hamlib only, when allowed), between bursts; a rig not
    // open now gets it once it is.
    void setFilter(const ragtime::radio::RigFilter& filter);
    // Why the last write failed, if it did.
    std::string filterProblem() const;
    // The rig, for the Ear's presets and calibrations: "hamlib-3070".
    std::string rigKey() const;
    // "Hamlib" or "rigctld": how the meters are read, for the tooltips.
    const char* meterPath() const;

    // The SWR trip (Settings > Rig): tripped until the operator clears it,
    // and what was done, "SWR 3.6: Ragtime unkeyed the rig and set its
    // power to 0 (it was 50%)". Clearing lets the rig be keyed again; its
    // power stays where the trip put it.
    bool swrTripped() const { return tripped_; }
    const std::string& tripText() const { return tripText_; }
    void clearSwrTrip();

private:
    void onTimer(wxTimerEvent& event);
    void sync();
    // The meters and the SWR trip from rigctld.
    void syncRigctld();
    // An S-meter reading between bursts; a new band starts afresh.
    void noiseReading(double dbOverS9, uint64_t nowMs);

    std::function<void()> changed_;
    std::function<void()> metersChanged_;
    std::function<void(const std::string&)> trippedChanged_;
    std::unique_ptr<RigConnection> rigctld_;
    ragtime::rig::HamlibRig hamlib_;
    ragtime::radio::PttFollower follower_;
    ragtime::radio::TxMeters meters_;
    ragtime::radio::NoiseFloor noise_;
    std::string noiseBand_;         // the band noise_ was read on
    // Through rigctld the trip only stops data2g-host: Ragtime sends a
    // shared rigctld no set commands, so it never unkeys or turns down.
    ragtime::radio::SwrTrip rigctldTrip_;
    ragtime::radio::RigSettings settings_;
    bool started_ = false;
    bool hostPtt_ = false;
    bool filterWatch_ = false;
    bool tripped_ = false;
    std::string tripText_;
    wxTimer timer_;
    uint64_t seenVersion_ = 0;
    uint64_t seenFailures_ = 0;
    uint64_t seenFollower_ = 0;
    uint64_t seenAlc_ = 0;
    uint64_t seenSwr_ = 0;
    uint64_t seenPower_ = 0;
    uint64_t seenTrips_ = 0;
    uint64_t seenStrength_ = 0;
};

#endif
