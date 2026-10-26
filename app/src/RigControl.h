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
#include "ragtime/rig/HamlibRig.h"

class RigConnection;

// The radio as Settings > Rig has it: nothing, the dial read from rigctld
// (RigConnection, read only), or the rig opened with Hamlib
// (rig::HamlibRig), which follows data2g-host's PTT when "Key the rig when
// data2g-host keys" is ticked (radio::PttFollower: off by default, and
// unkeyed if the host link drops, the rig fails, Ragtime closes, or after
// 180 s keyed). The window asks it the dial, whichever way it is read.
class RigControl : public wxEvtHandler
{
public:
    // Called when the reading, the connection or the keying changes.
    explicit RigControl(std::function<void()> changed);
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

private:
    void onTimer(wxTimerEvent& event);
    void sync();

    std::function<void()> changed_;
    std::unique_ptr<RigConnection> rigctld_;
    ragtime::rig::HamlibRig hamlib_;
    ragtime::radio::PttFollower follower_;
    ragtime::radio::RigSettings settings_;
    bool started_ = false;
    wxTimer timer_;
    uint64_t seenVersion_ = 0;
    uint64_t seenFailures_ = 0;
    uint64_t seenFollower_ = 0;
};

#endif
