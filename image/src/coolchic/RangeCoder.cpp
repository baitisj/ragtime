// Ragtime's range coder for Cool-chic's latents (docs/PICTURE_CODEC.md,
// "Latent coding"). MIT, as the rest of Ragtime.

#include "RangeCoder.h"

#include <cstddef>
#include <cstdint>

#include "RangeTables.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

constexpr uint64_t TOP = uint64_t(1) << 56;     // the range stays in [2^56, 2^64)

// Half of e^(-|d| / b) in 1/2^24ths, d in 1/256ths from mu, k = K[s].
uint32_t halfTail(int64_t d, uint32_t k)
{
    const uint64_t e = uint64_t(d < 0 ? -d : d) * uint64_t(k);
    const uint64_t ip = e >> 32;
    if (ip >= 24)
        return 0;
    return POW_TABLE[(e >> 22) & 1023] >> (8 + ip);
}

} // namespace

uint32_t cumulative(int i, int m, int s)
{
    if (i <= 0)
        return 0;
    if (i >= SYMBOLS)
        return FREQ_TOTAL;
    // The edge between values i - 65 and i - 64 is at i - 64.5.
    const int64_t d = int64_t(i - SYMBOLS / 2) * 256 - 128 - (int64_t(m) - 16384);
    const uint32_t t = halfTail(d, SCALE_TABLE[s]);
    const uint64_t g = d < 0 ? t : FREQ_TOTAL - t;
    return uint32_t(i) + uint32_t(((uint64_t(FREQ_TOTAL) - SYMBOLS) * g) >> FREQ_BITS);
}

uint8_t RangeDecoder::next()
{
    const uint8_t b = pos_ < length_ ? run_[pos_] : 0;
    pos_++;
    return b;
}

void RangeDecoder::start(const uint8_t* run, size_t length)
{
    run_ = run;
    length_ = length;
    pos_ = 0;
    range_ = ~uint64_t(0);
    code_ = 0;
    for (int i = 0; i < 8; i++)
        code_ = (code_ << 8) | next();
    started_ = true;
}

int RangeDecoder::decode(int m, int s)
{
    const uint64_t r = range_ >> FREQ_BITS;
    uint64_t v = code_ / r;
    if (v > FREQ_TOTAL - 1)
        v = FREQ_TOTAL - 1;
    int lo = 0, hi = SYMBOLS;
    while (hi - lo > 1)
    {
        const int mid = (lo + hi) / 2;
        if (cumulative(mid, m, s) <= v)
            lo = mid;
        else
            hi = mid;
    }
    const uint64_t c0 = cumulative(lo, m, s);
    const uint64_t c1 = cumulative(lo + 1, m, s);
    code_ -= r * c0;
    range_ = r * (c1 - c0);
    while (range_ < TOP)
    {
        range_ <<= 8;
        code_ = (code_ << 8) | next();
    }
    return lo - SYMBOLS / 2;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
