// AutoAlc: the connection bar's Auto ALC, which keeps the rig's ALC near
// 55% by moving data2g-host's transmit level (TXPOWER, Data2G PR #122).
//
// Each burst TxMeters works out the step from the median of its ALC
// readings (Burst::alcStepDb: down 2 dB over 80%, down 0.5 dB over 65%, up
// 0.5 dB under 45%, up 1 dB under 25%, nothing between 45% and 65%). With
// Auto ALC on, that is sent to the host when the burst ends. It never goes
// above the ceiling, which is the level the operator set (in Settings, or
// in data2g-host's own window) or found when ticked, never more than
// MAX_TRIM_DB under it, and never under TXPOWER's own floor.
//
// The host's level is the truth: Ragtime keeps no copy in its settings,
// and reads it back after every change (hostLevel). A level Ragtime didn't
// send (the operator's own, a restarted host) becomes the new ceiling. A
// burst that began before the level was set by hand was sent at another
// level, so it moves nothing; nor does any burst while the operator holds
// the knob (hold, Settings open). The host keeps a level after its client
// goes, so turning Auto ALC off, or quitting, puts back the ceiling
// (restore) if Auto ALC moved off it.
#ifndef RAGTIME_RADIO_AUTO_ALC_H
#define RAGTIME_RADIO_AUTO_ALC_H

#include <cstdint>
#include <deque>
#include <optional>

#include "ragtime/radio/TxMeters.h"

namespace ragtime
{
namespace radio
{

class AutoAlc
{
public:
    // Never more than this under the ceiling.
    static constexpr double MAX_TRIM_DB = 20.0;
    // TXPOWER's own floor (link::HostLink::TX_POWER_MIN_DB).
    static constexpr double LEVEL_MIN_DB = -60.0;

    // Auto ALC ticked or not. Turned off, the level to send to put back
    // the ceiling, if it moved off it.
    std::optional<double> setEnabled(bool on);
    bool enabled() const { return enabled_; }
    // The operator has the knob (Settings open): no steps meanwhile.
    void hold(bool on) { held_ = on; }

    // The host's level as it answered TXPOWER (on connecting, and after
    // each change); none with no host, or a host without TXPOWER.
    void hostLevel(std::optional<double> dB, uint64_t nowMs);
    // The operator set the level: the new ceiling, from nowMs.
    void operatorSet(double dB, uint64_t nowMs);
    // A level went to the host (TXPOWER), whoever chose it; its answer
    // is then ours.
    void sent(double dB);

    // A burst ended: the level to send, if Auto ALC moves it (then sent()).
    std::optional<double> burstEnded(const TxMeters::Burst& burst);
    // Quitting: the level to put back, if Auto ALC moved off it (then
    // sent()).
    std::optional<double> restore();

    std::optional<double> level() const { return level_; }
    // The most Auto ALC may use: the level set by hand, or found.
    std::optional<double> ceiling() const { return ceiling_; }
    // How far under the ceiling the level is now.
    double trimmedDb() const;
    // It has turned the level down MAX_TRIM_DB and goes no further.
    bool atFloor() const;
    // It is at the ceiling and the ALC still wants more.
    bool atCeiling() const { return wantsMore_; }

    // Goes up with any change.
    uint64_t version() const { return version_; }

private:
    // The newest level: the last one sent, until the host has answered
    // them all, or the host's.
    std::optional<double> newest() const;

    bool enabled_ = false;
    bool held_ = false;
    bool wantsMore_ = false;
    std::optional<double> level_;
    std::optional<double> ceiling_;
    std::deque<double> sent_;       // levels sent, oldest first, until the host answers each
    uint64_t setMs_ = 0;            // the level last set by hand, or from outside
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
