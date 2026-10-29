// Ragtime's range coder for Cool-chic's latents (docs/PICTURE_CODEC.md,
// "Latent coding"): a Laplace model in fixed point from two integer tables,
// and LZMA's range coder widened to a 64-bit range. Integers only, so every
// station decodes the same values. New for Ragtime, not ported: MIT, as the
// rest of Ragtime. tools/codecs/rcc/rcc.py is the reference.
#ifndef RAGTIME_IMAGE_COOLCHIC_RANGE_CODER_H
#define RAGTIME_IMAGE_COOLCHIC_RANGE_CODER_H

#include <cstddef>
#include <cstdint>

namespace ragtime
{
namespace image
{
namespace coolchic
{

constexpr int FREQ_BITS = 24;                   // frequencies sum to 2^24
constexpr uint32_t FREQ_TOTAL = uint32_t(1) << FREQ_BITS;
constexpr int SYMBOLS = 128;                    // values -64 to 63
constexpr int MU_INDICES = 32768;               // mu = -64 + m / 256
constexpr int SCALE_INDICES = 2561;             // ln b = -5 + s / 256

// The cumulative frequency below symbol index i (value i - 64), i = 0..128,
// for mu index m and scale index s, both already clamped.
uint32_t cumulative(int i, int m, int s);

// The ARM's indices, clamped as the spec says.
inline int clampMu(int64_t m)
{
    return m < 0 ? 0 : m >= MU_INDICES ? MU_INDICES - 1 : int(m);
}
inline int clampScale(int64_t s)
{
    return s < 0 ? 0 : s >= SCALE_INDICES ? SCALE_INDICES - 1 : int(s);
}

// Reads one grid's run. Bytes past the end of what it was given read as 0;
// pos() counts every byte read, so a caller can tell whether a value came
// only from bytes that have arrived.
class RangeDecoder
{
public:
    // run: the grid's bytes that have arrived, length of them.
    void start(const uint8_t* run, size_t length);
    // The value (-64 to 63) for mu index m and scale index s (clamped).
    int decode(int m, int s);
    size_t pos() const { return pos_; }
    // Whether the decoder has been started.
    bool started() const { return started_; }

    // More of the same run has arrived: the bytes are the same, only longer.
    void extend(const uint8_t* run, size_t length)
    {
        run_ = run;
        length_ = length;
    }

private:
    uint8_t next();

    const uint8_t* run_ = nullptr;
    size_t length_ = 0;
    size_t pos_ = 0;
    uint64_t range_ = 0;
    uint64_t code_ = 0;
    bool started_ = false;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
