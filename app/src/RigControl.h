#ifndef RAGTIME_RIG_CONTROL_H
#define RAGTIME_RIG_CONTROL_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <wx/event.h>
#include <wx/timer.h>

#include "ragtime/radio/PttFollower.h"
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
// and, from a Hamlib rig, the transmit meters (radio::TxMeters) read while
// data2g-host transmits.
class RigControl : public wxEvtHandler
{
public:
    // changed: the reading, the connection or the keying changed.
    // meters: a transmit meter reading came in, or a burst began or ended.
    RigControl(std::function<void()> changed, std::function<void()> meters);
    ~RigControl() override;

    // Starts, restarts or stops what the settings name; a rig already open
    // the same way stays open.
    void apply(const ragtime::radio::RigSettings& settings);
    // Unkeys and closes, for quitting.
    void stop();

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
    bool readsMeters() const;

private:
    void onTimer(wxTimerEvent& event);
    void sync();

    std::function<void()> changed_;
    std::function<void()> metersChanged_;
    std::unique_ptr<RigConnection> rigctld_;
    ragtime::rig::HamlibRig hamlib_;
    ragtime::radio::PttFollower follower_;
    ragtime::radio::TxMeters meters_;
    ragtime::radio::RigSettings settings_;
    bool started_ = false;
    bool hostPtt_ = false;
    wxTimer timer_;
    uint64_t seenVersion_ = 0;
    uint64_t seenFailures_ = 0;
    uint64_t seenFollower_ = 0;
    uint64_t seenAlc_ = 0;
    uint64_t seenSwr_ = 0;
    uint64_t seenPower_ = 0;
};

#endif
