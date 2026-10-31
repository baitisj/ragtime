// MicCheck: what Settings > Voice's Test says about three seconds from the
// microphone, before any processing (docs/STAGE1.md, "Voice audio").
//
// It measures as sotto does, by RMS, never sample peaks: sotto codes each
// frame's RMS energy, saturating above -7.3 dBFS of 10 ms RMS
// (sotto.h's ceiling). The loudest 10 ms is where the voice peaks, and
// sotto's test voices peak at -15 to -11 dBFS; the quietest tenth of the
// 20 ms frames is the room between words. The deck's gain knob and
// leveller fix a lot, but a microphone 30 dB down, or one buried in its
// own hiss, is better fixed at the desk.
#ifndef RAGTIME_VOICE_MIC_CHECK_H
#define RAGTIME_VOICE_MIC_CHECK_H

#include <cstddef>
#include <string>

namespace ragtime
{
namespace voice
{

// sotto's ceiling, and where its test voices peak (sotto.h).
constexpr double SOTTO_CEILING_DBFS = -7.3;
constexpr double PEAK_LOW_DBFS = -15.0;
constexpr double PEAK_HIGH_DBFS = -11.0;
// Quieter than this at its loudest, the microphone is barely hearing you.
constexpr double TOO_QUIET_DBFS = -40.0;
// The room should be this far under the voice.
constexpr double GOOD_NOISE_GAP_DB = 30.0;

struct MicCheck
{
    double loudestDbfs = -120.0;    // the loudest 10 ms, RMS
    double noiseDbfs = -120.0;      // the quietest tenth of 20 ms frames, RMS
    bool clipped = false;           // a sample at full scale
    enum class Verdict
    {
        Silent,     // nothing came, or digital silence
        TooQuiet,
        Quiet,      // workable: the gain knob will fix it
        Good,
        Loud,       // workable: over sotto's test voices
        TooLoud,    // over sotto's ceiling, or clipping
    } verdict = Verdict::Silent;
    bool noisy = false;             // the room is under 30 dB below the voice

    // One line for the Test's result: "Loudest -13 dBFS, room -58 dBFS:
    // good".
    std::string summary() const;
};

MicCheck checkMicrophone(const float* samples, std::size_t n, int rate);

} // namespace voice
} // namespace ragtime

#endif
