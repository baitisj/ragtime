// AutoAlc: the connection bar's Auto ALC, which keeps data2g-host's
// transmit level (TXPOWER, Data2G PR #122) just under the rig's ALC.
//
// Each burst TxMeters works out how far Glissando's rule would turn the
// drive down (Burst::turnDownDb: 0.5 dB for two ALC readings in a row over
// half the ALC scale, 2 dB for one over 0.8). With Auto ALC on, that is
// sent to the host when the burst ends. It only ever turns the level down:
// never above the level it found (the ceiling), never more than
// MAX_TRIM_DB under it, and never under TXPOWER's own floor.
//
// The host's level is the truth: Ragtime keeps no copy in its settings,
// and reads it back after every change (hostLevel). A level Ragtime didn't
// send (the operator's own, a restarted host) becomes the new ceiling.
// The host keeps a level after its client goes, so turning Auto ALC off,
// or quitting, puts back the ceiling (restore) if Auto ALC lowered it.
#ifndef RAGTIME_RADIO_AUTO_ALC_H
#define RAGTIME_RADIO_AUTO_ALC_H

#include <cstdint>
#include <optional>

#include "ragtime/radio/TxMeters.h"

namespace ragtime
{
namespace radio
{

class AutoAlc
{
public:
    // Never more than this under the level it found.
    static constexpr double MAX_TRIM_DB = 20.0;
    // TXPOWER's own floor (link::HostLink::TX_POWER_MIN_DB).
    static constexpr double LEVEL_MIN_DB = -60.0;

    // Auto ALC ticked or not. Turned off, the level to send to put back
    // the one it found, if it lowered it.
    std::optional<double> setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // The host's level as it answered TXPOWER (on connecting, and after
    // each change); none with no host, or a host without TXPOWER.
    void hostLevel(std::optional<double> dB);
    // The operator set the level: the new ceiling.
    void operatorSet(double dB);

    // A burst ended: the level to send, if Auto ALC turns it down.
    std::optional<double> burstEnded(const TxMeters::Burst& burst);
    // Quitting: the level to put back, if Auto ALC lowered it.
    std::optional<double> restore();

    std::optional<double> level() const { return level_; }
    // The level Auto ALC found and won't go above.
    std::optional<double> ceiling() const { return ceiling_; }
    // How far under the ceiling the level is now.
    double trimmedDb() const;
    // It has turned the level down MAX_TRIM_DB and goes no further.
    bool atFloor() const;

    // Goes up with any change.
    uint64_t version() const { return version_; }

private:
    bool enabled_ = false;
    std::optional<double> level_;
    std::optional<double> ceiling_;
    std::optional<double> expected_;    // the level last sent, until the host answers it
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
