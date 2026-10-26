// TxMeters: the rig's transmit meters, read through Hamlib or rigctld
// while data2g-host transmits (Settings > Rig), gathered burst by burst
// for the connection bar's ALC meter and the flight recorder.
//
// A reading has whichever of ALC, SWR and RF power the rig reports. ALC
// is Hamlib's float, a share of the rig's own ALC scale (an Icom's 1.0 is
// the top of its ALC zone); SWR is the ratio; power is in watts where the
// backend knows them, or else a share of full power.
//
// It only reads; Ragtime never touches the sound card, and data2g-host
// takes no transmit level command yet, so nothing turns the drive down.
// But each burst works out what Auto ALC would have done, by Glissando's
// rule (its Drive knob, docs/APP.md there): two ALC readings in a row over
// ALC_TARGET turn the drive down STEP_DB, one over ALC_HIGH turns it down
// BIG_STEP_DB. When the host gains its command, that sum is what to send.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_RADIO_TX_METERS_H
#define RAGTIME_RADIO_TX_METERS_H

#include <cstdint>
#include <optional>
#include <string>

namespace ragtime
{
namespace radio
{

struct MeterReading
{
    std::optional<double> alc;      // share of the rig's ALC scale
    std::optional<double> swr;      // 1.0 and up
    std::optional<double> watts;    // RF out, where the backend knows watts
    std::optional<double> power;    // RF out as a share of full power, if not

    bool any() const { return alc || swr || watts || power; }
};

class TxMeters
{
public:
    static constexpr double ALC_TARGET = 0.5;
    static constexpr double ALC_HIGH = 0.8;
    static constexpr double SWR_HIGH = 3.0;
    static constexpr double STEP_DB = 0.5;
    static constexpr double BIG_STEP_DB = 2.0;

    // One keying, from PTT on to PTT off.
    struct Burst
    {
        uint64_t startMs = 0;
        uint64_t endMs = 0;
        int readings = 0;
        std::optional<double> alcPeak;
        std::optional<double> alcMean;
        std::optional<double> swrPeak;
        std::optional<double> wattsMean;
        std::optional<double> powerMean;
        // What Auto ALC would have turned the drive down by.
        double turnDownDb = 0;

        bool overTarget() const { return alcPeak && *alcPeak > ALC_TARGET; }
        bool swrHigh() const { return swrPeak && *swrPeak > SWR_HIGH; }
    };

    // The rig keyed or unkeyed (data2g-host's PTT ON or OFF). A burst that
    // ends with readings becomes last().
    void transmitting(bool on, uint64_t nowMs);
    // A reading from the rig; ignored while not transmitting. Only the
    // meters it has change in latest().
    void reading(const MeterReading& reading, uint64_t nowMs);

    bool transmitting() const { return on_; }
    // The newest of each meter this burst; empty between bursts.
    const MeterReading& latest() const { return latest_; }
    // The burst under way.
    const Burst& current() const { return current_; }
    // The last burst that had readings.
    const std::optional<Burst>& last() const { return last_; }
    // Goes up when a burst ends with readings, for the flight recorder.
    uint64_t bursts() const { return bursts_; }

    // Goes up with any change.
    uint64_t version() const { return version_; }

private:
    bool on_ = false;
    MeterReading latest_;
    Burst current_;
    std::optional<Burst> last_;
    double alcSum_ = 0, wattsSum_ = 0, powerSum_ = 0;
    int alcCount_ = 0, wattsCount_ = 0, powerCount_ = 0;
    int overInARow_ = 0;
    uint64_t bursts_ = 0;
    uint64_t version_ = 0;
};

// Where an SWR sits on the connection bar's SWR bar, 0 to 1. The bar is
// drawn on the reflection coefficient, |Γ| = (SWR - 1) / (SWR + 1), as a
// rig's own SWR meter is: 1:1 at the left end, 1.5 a third of the way
// (0.3), 2 at half way, 3 at three quarters, and 5 (Γ = 2/3) at the right
// end. An SWR under 1 (a rig not transmitting, Hamlib's dummy rig) is 0.
constexpr double SWR_FULL_SCALE = 5.0;
double swrScale(double swr);

// The bar's colour: green under 1.5, yellow to 2, orange to 3, red over 3
// (Jeff, 2026-10-26).
enum class SwrBand
{
    Good,
    Fair,
    Poor,
    Bad,
};
SwrBand swrBand(double swr);

// SwrTrip: Settings > Rig's "Over 3:1 SWR, stop data2g-host; with Hamlib
// also unkey the rig and turn its power to 0". It trips on READINGS
// readings in a row over TRIP_SWR within a burst, so one noisy reading
// doesn't, and stays tripped until clear(): a Hamlib rig's thread then
// unkeys, sets the power to 0 and keys no more, and the power is never put
// back by itself; through rigctld only data2g-host is stopped.
class SwrTrip
{
public:
    static constexpr double TRIP_SWR = 3.0;
    static constexpr int READINGS = 2;

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    // A burst begins or ends: the count starts afresh.
    void transmitting(bool on);
    // An SWR reading while transmitting; true when it trips now.
    bool reading(double swr, uint64_t nowMs);
    void clear();

    bool tripped() const { return tripped_; }
    double trippedSwr() const { return trippedSwr_; }
    uint64_t trippedMs() const { return trippedMs_; }

private:
    bool enabled_ = false;
    bool tripped_ = false;
    int over_ = 0;
    double trippedSwr_ = 0;
    uint64_t trippedMs_ = 0;
};

// "ALC 42%", "SWR 1.3", "38 W", or "31% power", joined with " · ".
std::string readingText(const MeterReading& reading);
// "12 s: ALC peak 61%, mean 44% · SWR 1.3 · 38 W", for the tooltip; the
// flight recorder, plain ASCII, passes "; " between the meters.
std::string burstText(const TxMeters::Burst& burst, const std::string& between = " \xc2\xb7 ");

} // namespace radio
} // namespace ragtime

#endif
