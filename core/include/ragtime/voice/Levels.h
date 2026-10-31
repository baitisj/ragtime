// Voice levels (docs/STAGE1.md, "Voice: the processing chain"): sotto's
// level figures, the active speech level of ITU-T P.56, a live version of it
// for meters, and the Level check a clip gets once it is recorded.
//
// Samples are floats where 1.0 is full scale (32768 in sotto's 16-bit
// samples), and levels are dBFS of RMS, 20 log10(rms), as in sotto.h: a
// full-scale sine is -3 dBFS.
#ifndef RAGTIME_VOICE_LEVELS_H
#define RAGTIME_VOICE_LEVELS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ragtime
{
namespace voice
{

// sotto's figures, from sotto.h (branch claude/c-port-interleave, a64de50;
// unchanged in v3.6, c10e134). From v3.6 sotto's energy field codes the
// 200-3700 Hz band the decoder rebuilds, which is never more than the full
// band's RMS measured here, so these figures err on the safe side.
// The active speech level sotto is tuned for (ITU-T P.56):
constexpr double LEVEL_TARGET_DBFS = -26.0;
// 10 ms RMS above this saturates sotto's energy field (code 31):
constexpr double LEVEL_MAX_DBFS = -7.3;
// 10 ms RMS below this is sent as silence (code 0):
constexpr double LEVEL_SILENT_DBFS = -76.2;

// Where the push-pull Target knob, popped out, starts turning a voice down:
// a 10 ms burst over this, 3 dB short of sotto's ceiling.
constexpr double PEAK_MODE_DBFS = -10.0;

// The quietest level a dB figure here reports, for silence.
constexpr double LEVEL_FLOOR_DBFS = -120.0;

// dBFS of a mean square (power), never below LEVEL_FLOOR_DBFS.
double powerDb(double meanSquare);

// The active speech level of ITU-T P.56, method B, measured over everything
// fed since the last reset: the RMS of the samples in speech, pauses left
// out. Speech is where an envelope of the signal (30 ms) stands above a
// threshold, and stays so for 200 ms after (the hangover); the threshold is
// the one that sits 15.9 dB under the level it finds.
class ActiveLevel
{
public:
    explicit ActiveLevel(int sampleRate);

    void reset();
    void add(const float* samples, size_t count);

    // The active level in dBFS (LEVEL_FLOOR_DBFS for silence).
    double levelDb() const;
    // The share of samples in speech, 0 to 1.
    double activity() const;
    // RMS over everything, pauses included.
    double longTermDb() const;
    uint64_t samples() const { return count_; }

private:
    static constexpr int THRESHOLDS = 16;  // 2^-15 .. 2^0
    void result(double& level, double& activity) const;

    double g_;          // envelope smoothing per sample
    uint64_t hangover_; // samples
    double p_ = 0, q_ = 0;
    double sumSquares_ = 0;
    uint64_t count_ = 0;
    std::array<uint64_t, THRESHOLDS> active_{};
    std::array<uint64_t, THRESHOLDS> hang_{};
};

// A live active level for the meters, from 10 ms blocks: blocks above a
// gate count as speech (with P.56's 200 ms hangover), and their power is
// averaged over the last few seconds of speech. The gate sits 15.9 dB
// under the level found, as P.56's threshold does, and above the noise
// floor; it reads within a dB or so of ActiveLevel on speech, and holds
// still through pauses.
class LiveLevel
{
public:
    // timeConstant: seconds of speech the average spans.
    explicit LiveLevel(double timeConstant = 2.0);

    void reset();
    // One 10 ms block's mean square. Returns whether it counted as speech.
    bool addBlock(double meanSquare);

    // LEVEL_FLOOR_DBFS until speech is heard.
    double levelDb() const;
    bool heard() const { return blocks_ > 0; }
    // Whether the last block stood over the gate itself (speech, not the
    // hangover after it).
    bool voiced() const { return voiced_; }
    double noiseFloorDb() const { return noiseDb_; }

private:
    double alpha_;
    double power_ = 0;
    uint64_t blocks_ = 0;
    double noiseDb_ = 0;
    bool started_ = false;
    int hang_ = 0;
    bool voiced_ = false;
};

// The three lamps of the Level check.
enum class Lamp
{
    Green,  // on target
    Amber,  // near it
    Red,    // off it
};

const char* lampName(Lamp lamp);

// A clip's Level check: what sotto was given, measured at 8 kHz.
//   Active:  P.56 active level within 3 dB of the target (amber within 6).
//   Loudest: every 10 ms under sotto's ceiling before the limiter, and the
//            microphone never clipped (amber: the limiter took up to 3 dB).
//   Noise:   the quietest tenth of the clip's 20 ms frames at least 30 dB
//            under the voice (amber from 20 dB).
struct LevelReport
{
    double activeDb = LEVEL_FLOOR_DBFS;       // P.56, after the limiter
    double activity = 0;                      // share of the clip in speech
    double loudestDb = LEVEL_FLOOR_DBFS;      // loudest 10 ms, as sotto gets it
    double loudestBeforeLimiterDb = LEVEL_FLOOR_DBFS;
    double noiseDb = LEVEL_FLOOR_DBFS;        // quietest tenth of 20 ms frames
    double marginDb = 0;                      // activeDb - noiseDb
    uint64_t clippedSamples = 0;              // at the microphone, full scale
    double seconds = 0;
    bool speech = false;                      // any speech found at all

    Lamp active = Lamp::Red;
    Lamp loudest = Lamp::Red;
    Lamp noise = Lamp::Red;
};

class LevelCheck
{
public:
    static constexpr int RATE = 8000;

    explicit LevelCheck(double targetDb = LEVEL_TARGET_DBFS);

    void reset();
    void setTarget(double targetDb) { target_ = targetDb; }
    // Samples as sotto gets them (after the limiter) and as they were
    // before it, side by side and the same length.
    void add(const float* out, const float* beforeLimiter, size_t count);
    // Samples that hit full scale at the microphone.
    void addClipped(uint64_t samples) { clipped_ += samples; }

    LevelReport report() const;

private:
    double target_;
    ActiveLevel active_;
    // Loudest 10 ms, in windows of 80 samples every 40.
    std::vector<float> tail_, tailBefore_;
    double loudest_ = 0, loudestBefore_ = 0;
    // 20 ms frames' mean squares, for the noise.
    std::vector<double> frames_;
    double frameSum_ = 0;
    int frameFill_ = 0;
    uint64_t clipped_ = 0;
    uint64_t count_ = 0;
};

// Lamps from the numbers, as LevelCheck::report() sets them.
Lamp activeLamp(double activeDb, double targetDb, bool speech);
Lamp loudestLamp(double loudestBeforeLimiterDb, uint64_t clippedSamples);
Lamp noiseLamp(double marginDb, bool speech);

} // namespace voice
} // namespace ragtime

#endif
