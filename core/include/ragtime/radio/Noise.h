// Noise: the band's noise, from the rig's S-meter read between bursts
// (Hamlib's STRENGTH level, in dB over S9, through rigctld or Hamlib), for
// the gearshift's "QRN: -40" (the rig's own number) and its S-unit axis.
//
// The S-meter is read between bursts only: while we transmit it would read
// our own signal. It reads whatever is in the passband, a station's burst as
// readily as the noise, so the noise is the quietest reading of the last
// WINDOW_MS. With no reading for STALE_MS it is no longer known.
//
// S-units are the ham's: S9 is 0 dB here, and each S-unit below it 6 dB.
// Few S-meters are that exact, so it is a guide, not a measurement.
//
// What a station at some S/N reads on the S-meter. Data2G's S/N is the
// signal over the noise in 2500 Hz; the S-meter reads the signal and the
// noise together, as powers, over the whole receive filter. With noise N
// in the filter's B Hz, the noise in 2500 Hz is N x 2500/B, so a signal
// at S/N x lifts the needle to
//
//     N + 10 log10(1 + 10^(x/10) x 2500/B)  dB.
//
// A station well over the noise reads N + x + 10 log10(2500/B), the S/N
// moved by the filter's width; one under it hardly moves the needle at
// all (S/N 0 in a 3000 Hz filter, under 3 dB). Data2G copies down to
// -12 dB, so its slowest gears copy stations the S-meter can't show.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_RADIO_NOISE_H
#define RAGTIME_RADIO_NOISE_H

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace ragtime
{
namespace radio
{

constexpr double DB_PER_S_UNIT = 6.0;
// Data2G's S/N is in this bandwidth.
constexpr double SNR_BANDWIDTH_HZ = 2500.0;
// A needle that moves under half an S-unit hardly shows it.
constexpr double NEEDLE_DB = 3.0;

// dB over S9 as S-units: 0 is S9, -36 is S3.
double sUnits(double dbOverS9);
// "S3", "S9", "S9+10" (to the nearest 5 dB over S9), "S0" under S1.
std::string sUnitText(double dbOverS9);
// The reading as the rig gave it, to the nearest dB: "-40", "0", "+12".
std::string levelText(double dbOverS9);

// How far over the noise the S-meter reads with a station at this S/N
// (Data2G's, in 2500 Hz) and a filter this wide; a width of 0 or less is
// taken as 2500 Hz.
double meterRiseDb(double snrDb, double filterHz);
// The S/N that lifts the S-meter this far over the noise; none for 0 dB or
// less, which no station reaches.
std::optional<double> snrForRise(double riseDb, double filterHz);

class NoiseFloor
{
public:
    static constexpr uint64_t WINDOW_MS = 60000;
    static constexpr uint64_t STALE_MS = 30000;
    // The S-meter is read this often between bursts, and not this soon
    // after one, while the receiver's AGC recovers.
    static constexpr uint64_t READ_MS = 5000;
    static constexpr uint64_t SETTLE_MS = 1500;

    // A reading taken while not transmitting.
    void reading(double dbOverS9, uint64_t nowMs);
    void clear();

    // The quietest reading of the last WINDOW_MS, in dB over S9.
    std::optional<double> dbOverS9(uint64_t nowMs) const;

    // Goes up with every reading.
    uint64_t version() const { return version_; }

private:
    std::deque<std::pair<uint64_t, double>> readings_;
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
