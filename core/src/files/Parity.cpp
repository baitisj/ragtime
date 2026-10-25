#include "ragtime/files/Parity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace ragtime
{
namespace files
{
namespace parity
{

namespace
{

// GF(2^16) by log and antilog tables, on the primitive polynomial
// x^16 + x^12 + x^3 + x + 1.
constexpr uint32_t POLY = 0x1100B;
constexpr uint32_t ORDER = 65535;

struct Tables
{
    std::vector<uint16_t> exp;  // 2 * ORDER entries, so a sum of two logs needs no reduction
    std::vector<uint32_t> log;

    Tables()
        : exp(2 * ORDER)
        , log(65536, 0)
    {
        uint32_t v = 1;
        for (uint32_t i = 0; i < ORDER; i++)
        {
            exp[i] = (uint16_t)v;
            exp[i + ORDER] = (uint16_t)v;
            log[v] = i;
            v <<= 1;
            if (v & 0x10000) v ^= POLY;
        }
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

uint16_t mul(uint16_t a, uint16_t b)
{
    if (a == 0 || b == 0) return 0;
    const Tables& t = tables();
    return t.exp[t.log[a] + t.log[b]];
}

uint16_t inv(uint16_t a)
{
    const Tables& t = tables();
    return t.exp[ORDER - t.log[a]];
}

uint16_t word(const std::vector<uint8_t>& bytes, size_t w)
{
    size_t at = 2 * w;
    uint16_t hi = at < bytes.size() ? bytes[at] : 0;
    uint16_t lo = at + 1 < bytes.size() ? bytes[at + 1] : 0;
    return (uint16_t)(hi << 8 | lo);
}

// acc += c * d, word by word.
void addScaled(std::vector<uint16_t>& acc, uint16_t c, const std::vector<uint8_t>& d)
{
    if (c == 0) return;
    const Tables& t = tables();
    uint32_t lc = t.log[c];
    for (size_t w = 0; w < acc.size(); w++)
    {
        uint16_t v = word(d, w);
        if (v != 0) acc[w] ^= t.exp[lc + t.log[v]];
    }
}

void addScaledWords(std::vector<uint16_t>& acc, uint16_t c, const std::vector<uint16_t>& d)
{
    if (c == 0) return;
    const Tables& t = tables();
    uint32_t lc = t.log[c];
    for (size_t w = 0; w < acc.size(); w++)
    {
        if (d[w] != 0) acc[w] ^= t.exp[lc + t.log[d[w]]];
    }
}

std::vector<uint8_t> bytesOf(const std::vector<uint16_t>& words)
{
    std::vector<uint8_t> out(words.size() * 2);
    for (size_t w = 0; w < words.size(); w++)
    {
        out[2 * w] = (uint8_t)(words[w] >> 8);
        out[2 * w + 1] = (uint8_t)words[w];
    }
    return out;
}

} // namespace

std::vector<uint8_t> parityOf(uint16_t x, const std::vector<Symbol>& data, size_t length)
{
    if (length % 2 != 0) return {};
    std::vector<uint16_t> acc(length / 2, 0);
    for (const Symbol& d : data)
    {
        if (d.name == x || d.bytes == nullptr) return {};
        addScaled(acc, inv((uint16_t)(x ^ d.name)), *d.bytes);
    }
    return bytesOf(acc);
}

bool rebuild(const std::vector<Symbol>& known, const std::vector<Symbol>& parity,
             const std::vector<uint16_t>& missing, size_t length, std::vector<std::vector<uint8_t>>& out)
{
    out.clear();
    size_t m = missing.size();
    if (length % 2 != 0 || parity.size() < m) return false;
    if (m == 0) return true;
    std::set<uint16_t> names;
    for (const Symbol& s : known)
    {
        if (s.bytes == nullptr || !names.insert(s.name).second) return false;
    }
    for (uint16_t y : missing)
    {
        if (!names.insert(y).second) return false;
    }
    std::vector<uint16_t> xs;
    for (size_t r = 0; r < m; r++)
    {
        if (parity[r].bytes == nullptr || !names.insert(parity[r].name).second) return false;
        xs.push_back(parity[r].name);
    }
    const std::vector<uint16_t>& ys = missing;
    size_t words = length / 2;

    // Each parity row less what the known data put into it leaves
    // sum over the missing of 1 / (x_r + y_c) times d_c: a square Cauchy
    // system, C d = rhs.
    std::vector<std::vector<uint16_t>> rhs(m, std::vector<uint16_t>(words, 0));
    for (size_t r = 0; r < m; r++)
    {
        for (size_t w = 0; w < words; w++) rhs[r][w] = word(*parity[r].bytes, w);
        for (const Symbol& s : known) addScaled(rhs[r], inv((uint16_t)(xs[r] ^ s.name)), *s.bytes);
    }

    // A Cauchy matrix has its inverse in closed form, so this is m squared
    // work, not m cubed. With A(z) the product of (z + x_k) and B(z) that of
    // (z + y_k), in a field where minus is plus:
    //   inverse[c][r] = A(y_c) B(x_r) / (A'(x_r) B'(y_c) (x_r + y_c)),
    // where A'(x_r) is the product of (x_r + x_k) over k other than r, and
    // B'(y_c) that of (y_c + y_k) over k other than c.
    std::vector<uint16_t> ay(m, 1), bx(m, 1), ap(m, 1), bp(m, 1);
    for (size_t i = 0; i < m; i++)
    {
        for (size_t k = 0; k < m; k++)
        {
            ay[i] = mul(ay[i], (uint16_t)(ys[i] ^ xs[k]));
            bx[i] = mul(bx[i], (uint16_t)(xs[i] ^ ys[k]));
            if (k != i)
            {
                ap[i] = mul(ap[i], (uint16_t)(xs[i] ^ xs[k]));
                bp[i] = mul(bp[i], (uint16_t)(ys[i] ^ ys[k]));
            }
        }
    }
    for (size_t c = 0; c < m; c++)
    {
        std::vector<uint16_t> d(words, 0);
        uint16_t top = ay[c];
        uint16_t under = bp[c];
        for (size_t r = 0; r < m; r++)
        {
            uint16_t num = mul(top, bx[r]);
            uint16_t den = mul(mul(ap[r], under), (uint16_t)(xs[r] ^ ys[c]));
            addScaledWords(d, mul(num, inv(den)), rhs[r]);
        }
        out.push_back(bytesOf(d));
    }
    return true;
}

uint32_t wantFor(uint32_t need, double loss)
{
    if (need == 0) return 0;
    double p = std::min(0.75, std::max(0.0, loss));
    double want = ((double)need + std::sqrt((double)need * p / (1.0 - p))) / (1.0 - p);
    return (uint32_t)std::min(65535.0, std::ceil(want - 1e-9));
}

} // namespace parity
} // namespace files
} // namespace ragtime
