// The stages of the voice processing chain (docs/STAGE1.md, "Voice: the
// processing chain"), each on its own so it can be tested alone: the
// high-pass, the resampler, the leveller with its peak mode, and the
// limiter. voice::Chain (Chain.h) strings them together.
//
// Samples are floats where 1.0 is full scale (Levels.h).
#ifndef RAGTIME_VOICE_DSP_H
#define RAGTIME_VOICE_DSP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ragtime/voice/Levels.h"

namespace ragtime
{
namespace voice
{

// A fourth-order Butterworth high-pass (24 dB an octave), as two biquads:
// at 100 Hz it takes 60 Hz hum down 18 dB and 50 Hz down 24 dB, and leaves
// the voice above 200 Hz alone. Turned off it passes samples unchanged.
class HighPass
{
public:
    HighPass(double sampleRate, double cornerHz);

    // Changes the corner, keeping the filter's state (no click).
    void setCorner(double cornerHz);
    double corner() const { return corner_; }
    void reset();
    void process(float* samples, size_t count);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
    };
    double rate_;
    double corner_ = 0;
    Biquad stage_[2];
};

// A polyphase windowed-sinc resampler for any ratio of whole rates (48 kHz
// to 8 kHz, 44.1 kHz to 48 kHz, ...): Kaiser window, 80 dB down in the stop
// band, the pass band flat to 90% of the lower Nyquist frequency. It runs
// as samples come and delays them by delayIn() input samples.
class Resampler
{
public:
    Resampler(int inRate, int outRate);

    int inRate() const { return inRate_; }
    int outRate() const { return outRate_; }
    void reset();
    // Appends the output samples count input samples make.
    void process(const float* in, size_t count, std::vector<float>& out);
    // The filter's delay in input samples (feed this many zeros to drain).
    size_t delayIn() const { return delayIn_; }

private:
    int inRate_, outRate_;
    int up_ = 1, down_ = 1;      // L, M
    int taps_ = 1;               // per phase
    std::vector<float> h_;       // phase-major: h_[phase * taps_ + k]
    size_t delayIn_ = 0;
    std::vector<float> hist_;    // input samples from histStart_
    uint64_t histStart_ = 0;
    uint64_t inCount_ = 0;
    uint64_t outCount_ = 0;
};

// The limiter, last in the chain at 8 kHz, so it sees exactly what sotto
// gets. It keeps every 10 ms window's RMS under rmsCeilingDb (1 dB under
// sotto's ceiling by default) and every sample under peakCeilingDb (-1 dBFS),
// looking ahead so it turns down before a burst rather than during it: the
// gain falls over 5 ms and recovers with a 50 ms time constant. It never
// raises anything, and below its ceilings it leaves no trace. It delays
// samples by delay() samples.
class Limiter
{
public:
    explicit Limiter(int sampleRate = 8000, double rmsCeilingDb = LEVEL_MAX_DBFS - 1.0,
                     double peakCeilingDb = -1.0);

    void reset();
    // Appends the samples that come out (count of them, delay() behind).
    void process(const float* in, size_t count, std::vector<float>& out);
    size_t delay() const { return delay_; }
    // The most gain reduction applied since the last call, in dB (0 or more).
    double takeReductionDb();

private:
    int window_;      // R: samples in 10 ms
    int attack_;      // K: samples the gain falls over
    size_t delay_;
    float rmsCeiling_, peakCeiling_;
    float release_;   // per sample
    static constexpr int RING = 512;
    float x_[RING] = {};
    float rmsReq_[RING] = {};
    float q_[RING] = {};
    float m_[RING] = {};
    double sumSquares_ = 0;
    double mSum_ = 0;
    uint64_t n_ = 0;   // samples in
    float gain_ = 1.0f;
    float minGain_ = 1.0f;
};

// The leveller: a slow hand on the fader. It measures the active level of
// what it is given (LiveLevel's way, over a couple of seconds of speech) and
// moves its gain so that level comes out at the target, at most 3 dB a
// second up and 6 down, and only while someone is talking, so pauses don't
// pump the room up. For the first half second of speech after a reset it
// moves fast, so the first clip starts near the target; after that it keeps
// its gain from clip to clip, as a hand on a fader would.
//
// Peak mode (the Target knob popped out): a 10 ms block that would come out
// over PEAK_MODE_DBFS turns the gain down by its overshoot at once, and that
// cut then wears off at 1 dB a second of speech, slower than the leveller
// itself moves, so a voice that keeps shouting stays turned down rather than
// being limited syllable by syllable. Pushed back in, the cut goes.
class Leveller
{
public:
    // blockSize: samples in 10 ms at the rate it runs at.
    explicit Leveller(size_t blockSize);

    void reset();
    void setTarget(double targetDb) { target_ = targetDb; }
    double target() const { return target_; }
    void setPeakMode(bool on) { peakMode_ = on; }
    bool peakMode() const { return peakMode_; }

    // One 10 ms block, in place.
    void processBlock(float* block);

    // The gain it applies, peak mode's cut included.
    double gainDb() const { return gainDb_; }
    // What peak mode has taken off and not yet given back (0 or more).
    double peakCutDb() const { return peakCutDb_; }
    // The active level it measures coming in (LEVEL_FLOOR_DBFS until heard).
    double inputLevelDb() const { return level_.levelDb(); }
    // Whether the last block counted as speech.
    bool speaking() const { return speaking_; }
    // The dB the last block's peak mode took off (0 when it took nothing).
    double lastPeakCutDb() const { return lastPeakCut_; }

    static constexpr double MIN_GAIN_DB = -30.0;
    static constexpr double MAX_GAIN_DB = 30.0;
    static constexpr double UP_DB_PER_S = 3.0;
    static constexpr double DOWN_DB_PER_S = 6.0;
    static constexpr double FAST_DB_PER_S = 100.0;
    static constexpr double FAST_SECONDS = 0.5;
    static constexpr double PEAK_RECOVER_DB_PER_S = 1.0;

private:
    size_t block_;
    double target_;
    bool peakMode_ = false;
    LiveLevel level_;
    double gainDb_ = 0;
    double levelGainDb_ = 0;    // the leveller's own
    double peakCutDb_ = 0;      // peak mode's
    double appliedGain_ = 1.0;  // linear, at the end of the last block
    uint64_t speechBlocks_ = 0;
    bool speaking_ = false;
    double lastPeakCut_ = 0;
};

} // namespace voice
} // namespace ragtime

#endif
