// PttFollower: whether Ragtime should be keying the rig, when Settings >
// Rig has "Key the rig when data2g-host keys" ticked.
//
// data2g-host says "PTT ON" on its command port when it keys and "PTT
// OFF" when it unkeys (Data2G's Station::announce_ptt: ON as the keyer's
// lead-in starts, OFF once the audio has drained and the off delay
// passed), and it says so even when it was started with --no-rig and
// keys nothing itself. The follower keys at ON and unkeys at OFF, and
// unkeys on its own, so a lost message never leaves a transmitter on:
//
//   - the command link to data2g-host drops (hostLost),
//   - the rig fails (rigFailed),
//   - Ragtime closes or the option is turned off (setEnabled(false)),
//   - and in any case once it has been keyed MAX_KEYED_MS (180 s, the
//     usual transmit time-out), whatever the host says.
//
// After the time-out or a rig failure it stays unkeyed until the host
// says PTT OFF: the next PTT ON keys again. After the SWR trip (lockOut)
// it stays unkeyed, whatever the host says, until the operator clears it.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_RADIO_PTT_FOLLOWER_H
#define RAGTIME_RADIO_PTT_FOLLOWER_H

#include <cstdint>
#include <string>

namespace ragtime
{
namespace radio
{

class PttFollower
{
public:
    static constexpr uint64_t MAX_KEYED_MS = 180 * 1000;

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    // data2g-host's PTT ON or PTT OFF.
    void host(bool on, uint64_t nowMs);
    // The command link to data2g-host dropped.
    void hostLost();
    // Setting PTT failed, or the rig closed.
    void rigFailed(const std::string& why);
    // Unkeys after MAX_KEYED_MS.
    void tick(uint64_t nowMs);
    // The SWR trip: unkeyed, and kept unkeyed until clearLockOut().
    void lockOut(const std::string& why);
    void clearLockOut();
    bool lockedOut() const { return locked_; }

    // What the rig should be.
    bool keyed() const { return keyed_; }
    // Why it last unkeyed by itself, for the dial's tooltip: "data2g-host
    // kept PTT on for 180 s"; empty after a plain PTT OFF.
    const std::string& why() const { return why_; }

    // Goes up whenever keyed() changes.
    uint64_t version() const { return version_; }

private:
    void set(bool keyed);

    bool enabled_ = false;
    bool keyed_ = false;
    bool held_ = false;         // unkeyed by us: waiting for PTT OFF
    bool locked_ = false;       // the SWR trip: waiting for the operator
    uint64_t keyedMs_ = 0;
    std::string why_;
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
