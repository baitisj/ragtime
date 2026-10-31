// The voice processing chain (docs/STAGE1.md, "Voice: the processing
// chain"): what the microphone deck does to the voice before sotto codes
// it. In order:
//
//   input gain -> 48 kHz -> high-pass -> noise reduction -> leveller
//              -> 8 kHz -> limiter -> sotto
//
// The microphone comes in at its own rate and is resampled to 48 kHz,
// where RNNoise works, in 10 ms blocks; the end of the chain is 8 kHz,
// sotto's rate. The limiter runs after the resampler, so it sees exactly
// the samples sotto codes. Meters (Readings) are read along the way, and a
// clip gets its Level check (Levels.h) when it ends.
//
// The noise reducer is a Denoiser handed in: core links no libraries, and
// RNNoise lives in ragtime_denoise (denoise/). With none, the Noise stage
// passes the voice through.
//
// The audio thread owns a Chain and calls process(), startClip() and
// finish(). setSettings(), settings() and takeReadings() may be called from
// any thread (the window's): settings take effect at the next 10 ms block.
#ifndef RAGTIME_VOICE_CHAIN_H
#define RAGTIME_VOICE_CHAIN_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "ragtime/voice/Dsp.h"
#include "ragtime/voice/Levels.h"

namespace ragtime
{
namespace voice
{

// A noise reducer working on 10 ms frames at 48 kHz.
class Denoiser
{
public:
    static constexpr int RATE = 48000;
    static constexpr int FRAME = 480;

    virtual ~Denoiser() = default;
    // Cleans one frame in place, and returns how sure it is the frame holds
    // a voice, 0 to 1.
    virtual float process(float* frame) = 0;
    // How many samples its output lags its input.
    virtual int latency() const = 0;
    virtual void reset() = 0;
    // A name for the About box and logs, such as "RNNoise 0.1.1".
    virtual const char* name() const = 0;
};

// The deck's knobs, as the plan has them.
struct ChainSettings
{
    double inputGainDb = 0;        // Gain: -20 to +40 dB
    bool highPassIn = true;        // High-pass, pushed in
    double highPassHz = 100;       // 80, 100 or 150
    bool noiseIn = true;           // Noise, pushed in
    double noiseAmount = 0.70;     // 0 to 1
    double targetDb = LEVEL_TARGET_DBFS;  // Target: -36 to -16 dBFS
    bool peakMode = false;         // Target popped out

    // Clamped to the knobs' ranges.
    ChainSettings clamped() const;
};

// What the meters show. The "loudest", "clipped" and "reduction" fields
// cover everything since the last takeReadings(), so a window polling at
// 30 Hz misses no burst; it draws the needles' hold and fall itself.
struct Readings
{
    // At the input, after the Gain knob (the deck's Input meters).
    double inputActiveDb = LEVEL_FLOOR_DBFS;   // Active level (live)
    double inputLoudestDb = LEVEL_FLOOR_DBFS;  // loudest 10 ms
    bool clipped = false;                      // the mic hit full scale
    // The leveller and limiter (the gain-reduction meter).
    double levellerGainDb = 0;                 // + boost, - cut
    double limiterReductionDb = 0;             // the most, 0 or more
    bool speaking = false;                     // the leveller hears speech
    // From the noise reducer: how sure it is there is a voice, 0 to 1.
    float voiceProbability = 0;
    // At the output, as sotto gets it.
    double outputActiveDb = LEVEL_FLOOR_DBFS;
    double outputLoudestDb = LEVEL_FLOOR_DBFS;
};

class Chain
{
public:
    static constexpr int WORK_RATE = 48000;
    static constexpr int OUT_RATE = 8000;
    static constexpr size_t BLOCK = 480;  // 10 ms at WORK_RATE

    // inputRate: the microphone's rate. denoiser may be null.
    explicit Chain(int inputRate, std::unique_ptr<Denoiser> denoiser = nullptr);
    ~Chain();

    int inputRate() const { return inputRate_; }
    bool hasDenoiser() const { return denoiser_ != nullptr; }
    const Denoiser* denoiser() const { return denoiser_.get(); }

    // Any thread. Clamped to the knobs' ranges.
    void setSettings(const ChainSettings& settings);
    ChainSettings settings() const;

    // Feeds microphone samples and appends what comes out at 8 kHz, a fixed
    // latency() behind.
    void process(const float* in, size_t count, std::vector<float>& out);
    // Starts a clip's Level check afresh (Push to talk pressed). The
    // leveller keeps its gain from the last clip.
    void startClip();
    // Drains the latency with silence, appending the clip's last samples
    // (Push to talk let go). The Level check then covers the whole clip.
    void finish(std::vector<float>& out);
    LevelReport levelReport() const { return check_.report(); }

    // Any thread: the meters since the last call (see Readings).
    Readings takeReadings();
    // Everything back to how it was made, the leveller's gain included.
    void reset();

    // The delay from the microphone to the 8 kHz output, in seconds.
    double latencySeconds() const;

private:
    // The meters gathered on the audio thread since the last publish().
    struct CallMeters
    {
        double inLoudest = LEVEL_FLOOR_DBFS;
        double outLoudest = LEVEL_FLOOR_DBFS;
        bool clipped = false;
        float voiceProbability = 0;
    };

    void feed(const float* in, size_t count, std::vector<float>& out);
    void runBlock(float* block, std::vector<float>& out);
    void pickUpSettings();
    void publish();

    int inputRate_;
    std::unique_ptr<Denoiser> denoiser_;
    ChainSettings settings_;              // as the audio thread uses them
    mutable std::mutex mutex_;            // guards the two below
    ChainSettings nextSettings_;
    bool settingsChanged_ = false;
    Readings shared_;

    std::unique_ptr<Resampler> toWork_;   // null at 48 kHz already
    std::vector<float> work_;             // resampled input
    std::vector<float> pending_;          // waiting for a full block
    double inputGain_ = 1.0;              // linear, at the end of the last block
    LiveLevel inputLevel_;
    HighPass highPass_;
    std::vector<float> dry_;              // the denoiser's latency, for mixing
    size_t dryPos_ = 0;
    double noiseMix_ = 0;                 // the amount in use (ramps per block)
    Leveller leveller_;
    Resampler toOut_;
    std::vector<float> out8k_;
    Limiter limiter_;
    std::vector<float> limited_;
    // The 8 kHz samples before the limiter, lined up with its output.
    std::vector<float> beforeDelay_;
    LiveLevel outputLevel_;
    double outBlockSum_ = 0;
    int outBlockFill_ = 0;
    LevelCheck check_;
    CallMeters call_;
    // Samples in from the microphone and out at 8 kHz, ever; finish()
    // stops the output at outLimit_.
    uint64_t totalIn_ = 0;
    uint64_t totalOut_ = 0;
    uint64_t outLimit_ = 0;
};

// Floats to sotto's 16-bit samples (rounded, clamped to +-32767).
void toPcm16(const float* in, size_t count, int16_t* out);
// sotto's 16-bit samples to floats.
void fromPcm16(const int16_t* in, size_t count, float* out);

} // namespace voice
} // namespace ragtime

#endif
