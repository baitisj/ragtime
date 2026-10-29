// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). The weights' exp-Golomb code is ported to C++ for
// Ragtime from Cool-chic 5.0.1 (coolchic/bitstream/neuralnet/expgolomb.py).
// Authors: see Cool-chic's CONTRIBUTORS.md. The latent coding (the range
// encoder and the grid table) is Ragtime's own, from docs/PICTURE_CODEC.md.
#include "FitWrite.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Arm.h"
#include "Latents.h"
#include "RangeCoder.h"
#include "Stream.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

void BitWriter::put(uint64_t value, int bits)
{
    for (int b = bits - 1; b >= 0; b--)
    {
        if (bits_ % 8 == 0)
            bytes_.push_back(0);
        if ((value >> b) & 1)
            bytes_.back() |= (uint8_t)(0x80 >> (bits_ % 8));
        bits_++;
    }
}

std::vector<uint8_t> expGolombEncode(const std::vector<int32_t>& values,
                                     const std::vector<int>& orders, int& padBits)
{
    // Each code first, to count the bits the padding makes whole.
    struct Code
    {
        int zeros;
        uint64_t quotient;      // + 1, in zeros + 1 bits
        uint64_t remainder;
        int order;
    };
    std::vector<Code> codes;
    codes.reserve(values.size());
    size_t bits = 0;
    for (size_t i = 0; i < values.size(); i++)
    {
        const int64_t v = values[i];
        // Positive values odd, zero and negative ones even.
        const uint64_t u = v > 0 ? (uint64_t)(2 * v - 1) : (uint64_t)(-2 * v);
        const int k = orders[i];
        Code c;
        c.order = k;
        c.quotient = (u >> k) + 1;
        c.remainder = u & ((uint64_t(1) << k) - 1);
        c.zeros = 0;
        while ((c.quotient >> (c.zeros + 1)) != 0)
            c.zeros++;
        bits += (size_t)(2 * c.zeros + 1 + k);
        codes.push_back(c);
    }
    padBits = (int)((8 - bits % 8) % 8);
    BitWriter w;
    w.put(0, padBits);
    for (const Code& c : codes)
    {
        w.put(0, c.zeros);
        w.put(c.quotient, c.zeros + 1);
        w.put(c.remainder, c.order);
    }
    return w.bytes();
}

void RangeEncoder::add(uint64_t v)
{
    const uint64_t before = low_;
    low_ += v;
    carry_ += low_ < before ? 1 : 0;
}

void RangeEncoder::encode(int value, int mu, int scale)
{
    const int i = std::min(std::max(value, -SYMBOLS / 2), SYMBOLS / 2 - 1) + SYMBOLS / 2;
    const uint64_t lo = cumulative(i, mu, scale), hi = cumulative(i + 1, mu, scale);
    const uint64_t r = range_ >> FREQ_BITS;
    add(r * lo);
    range_ = r * (hi - lo);
    while (range_ < (uint64_t(1) << 56))
    {
        range_ <<= 8;
        shiftLow();
    }
}

void RangeEncoder::shiftLow()
{
    if (low_ < (uint64_t(0xFF) << 56) || carry_)
    {
        uint8_t temp = cache_;
        do
        {
            out_.push_back((uint8_t)(temp + carry_));
            temp = 0xFF;
        } while (--cacheSize_ != 0);
        cache_ = (uint8_t)(low_ >> 56);
    }
    cacheSize_++;
    low_ = (low_ & ((uint64_t(1) << 56) - 1)) << 8;
    carry_ = 0;
}

std::vector<uint8_t> RangeEncoder::finish()
{
    // The value in [low, low + range) with the most trailing zero bits, so
    // the bytes after it can be dropped.
    for (int bits = 64; bits >= 0; bits--)
    {
        const uint64_t mask = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
        uint64_t v = low_ + mask;
        int vCarry = carry_ + (v < low_ ? 1 : 0);
        v &= ~mask;
        if (bits == 64)
            v = 0;
        const uint64_t end = low_ + range_;
        const int endCarry = carry_ + (end < low_ ? 1 : 0);
        if (vCarry < endCarry || (vCarry == endCarry && v < end))
        {
            low_ = v;
            carry_ = vCarry;
            break;
        }
    }
    for (int i = 0; i < 9; i++)
        shiftLow();
    std::vector<uint8_t> run(out_.begin() + 1, out_.end());
    while (!run.empty() && run.back() == 0)
        run.pop_back();
    return run;
}

std::vector<uint8_t> packGrids(const std::vector<std::vector<uint8_t>>& runs)
{
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& run : runs)
    {
        uint64_t n = run.size();
        do
        {
            const uint8_t b = (uint8_t)(n & 0x7F);
            n >>= 7;
            out.push_back(n ? (uint8_t)(b | 0x80) : b);
        } while (n);
    }
    for (const std::vector<uint8_t>& run : runs)
        out.insert(out.end(), run.begin(), run.end());
    return out;
}

QuantisedNetwork sentNetwork(const std::vector<float>& params, const Quantiser& q)
{
    QuantisedNetwork n;
    for (int m = 0; m < MODULES; m++)
    {
        const NetQuant& nq = q.quant(m);
        n.weights[m] = q.sent(params, m, false);
        n.biases[m] = q.sent(params, m, true);
        n.weightShift[m] = nq.weightShift;
        n.biasShift[m] = nq.biasShift;
        n.weightExpGolomb[m] = nq.weightOrder;
        n.biasExpGolomb[m] = nq.biasOrder;
    }
    return n;
}

bool writeStream(const Model& model, const std::vector<float>& params, Net& net,
                 const Quantiser& q, std::vector<uint8_t>& out, std::string& error,
                 size_t* clamped)
{
    Architecture arch;
    if (!preset(model.arch.name, model.height, model.width, arch))
        return error = "no preset " + model.arch.name, false;
    if (!valid(arch, error))
        return false;
    const std::vector<Grid> gs = grids(arch);
    if (gs.size() != model.grids.size())
        return error = "the grids don't match the decoder's", false;
    for (size_t g = 0; g < gs.size(); g++)
        if (gs[g].height != model.grids[g].h || gs[g].width != model.grids[g].w ||
            gs[g].hyper != model.grids[g].hyper)
            return error = "the grids don't match the decoder's", false;

    // The weights, every module's in turn.
    const QuantisedNetwork qn = sentNetwork(params, q);
    std::vector<int32_t> values;
    std::vector<int> orders;
    for (int m = 0; m < MODULES; m++)
    {
        if (m == IFCE && arch.ifceOutputs == 0)
            continue;
        for (int b = 0; b < 2; b++)
        {
            const std::vector<int32_t>& list = b ? qn.biases[m] : qn.weights[m];
            if (list.size() != parameterCount(arch, Module(m), b == 1))
                return error = "the weights don't match the decoder's", false;
            values.insert(values.end(), list.begin(), list.end());
            orders.insert(orders.end(), list.size(),
                          b ? qn.biasExpGolomb[m] : qn.weightExpGolomb[m]);
        }
    }
    int padBits = 0;
    const std::vector<uint8_t> weights = expGolombEncode(values, orders, padBits);

    // The latents, as the decoder will see them.
    net.quantiseHard(params.data());
    const std::vector<float>& q32 = net.latents();
    std::vector<std::vector<int8_t>> latents(gs.size());
    size_t clamps = 0;
    for (size_t g = 0; g < gs.size(); g++)
    {
        const size_t count = (size_t)gs[g].height * (size_t)gs[g].width;
        const size_t at = net.gridOffset((int)g);
        latents[g].resize(count);
        for (size_t i = 0; i < count; i++)
        {
            const float v = q32[at + i];
            const float c = std::min(std::max(v, -64.0f), 63.0f);
            clamps += c != v ? 1 : 0;
            latents[g][i] = (int8_t)c;
        }
    }
    if (clamped)
        *clamped = clamps;

    FixedArm arm;
    std::vector<FixedArm> ifce;
    if (!fixedArm(arch, qn, arm, error) || !fixedIfce(arch, qn, ifce, error))
        return false;

    // Each grid's run, coarsest first, filled in coding order as the
    // decoder fills it.
    std::vector<std::vector<uint8_t>> runs;
    for (size_t k = gs.size(); k-- > 0;)
    {
        GridModel gm(arch, gs, latents, k, arm, ifce[k]);
        const int h = gs[k].height, w = gs[k].width;
        std::vector<int8_t> work((size_t)h * (size_t)w, 0);
        RangeEncoder enc;
        for (int32_t index : codingOrder(h, w))
        {
            int m = 0, s = 0;
            gm.indices(work.data(), index / w, index % w, m, s);
            const int8_t v = latents[k][(size_t)index];
            enc.encode(v, m, s);
            work[(size_t)index] = v;
        }
        runs.push_back(enc.finish());
    }
    const std::vector<uint8_t> latentPart = packGrids(runs);

    StreamHeader h;
    h.arch = arch;
    h.net = qn;
    h.weightBytes = weights.size();
    h.weightPadBits = padBits;
    h.latentBytes = latentPart.size();
    out.clear();
    if (!writeHeaders(h, out, error))
        return false;
    out.insert(out.end(), weights.begin(), weights.end());
    out.insert(out.end(), latentPart.begin(), latentPart.end());
    return true;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
