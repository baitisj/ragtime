// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/header/ (header.py, element.py) and
// coolchic/bitstream/neuralnet/ (neuralnet.py, expgolomb.py). The licence
// is LICENSE.txt beside this file.

#include "Stream.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

// Bits, most significant first, as Cool-chic's binary strings run.
class BitReader
{
public:
    BitReader(const uint8_t* bytes, size_t length, size_t startByte)
        : bytes_(bytes), bits_(uint64_t(length) * 8), pos_(uint64_t(startByte) * 8)
    {
    }

    uint64_t read(int n)
    {
        uint64_t v = 0;
        for (int i = 0; i < n; i++)
        {
            int bit = 0;
            if (pos_ < bits_)
                bit = (bytes_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
            else
                short_ = true;
            pos_++;
            v = (v << 1) | uint64_t(bit);
        }
        return v;
    }
    int bit() { return int(read(1)); }
    bool ranShort() const { return short_; }
    uint64_t pos() const { return pos_; }

private:
    const uint8_t* bytes_;
    uint64_t bits_;
    uint64_t pos_;
    bool short_ = false;
};

// Bits, most significant first, padded to whole bytes at the end.
class BitWriter
{
public:
    void write(uint64_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--)
        {
            if (bits_ % 8 == 0)
                bytes_.push_back(0);
            if ((v >> i) & 1)
                bytes_.back() |= uint8_t(0x80 >> (bits_ % 8));
            bits_++;
        }
    }
    uint64_t bits() const { return bits_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    uint64_t bits_ = 0;
};

bool fits(uint64_t v, int bits)
{
    return v < (uint64_t(1) << bits);
}

// Each header ends with n_bytes_header and is padded to whole bytes; the
// next starts n_bytes_header bytes after this one's start.
bool headerLength(BitReader& r, size_t start, size_t declared, size_t& next, std::string& error)
{
    const uint64_t bits = r.pos() - uint64_t(start) * 8;
    if (declared != (bits + 7) / 8)
        return error = "header length doesn't match", false;
    next = start + declared;
    return true;
}

// nnquant/quantstep.py's possible steps, as the shift of the first and how
// many there are: the header sends an index into them.
struct Steps
{
    int first;
    int count;
};
const Steps WEIGHT_STEPS[MODULES] = {{-8, 9}, {-8, 9}, {-12, 13}, {-12, 13}};
const Steps BIAS_STEPS[MODULES] = {{-16, 17}, {-16, 17}, {0, 1}, {-24, 25}};
constexpr int EXP_GOLOMB_ORDERS = 13;   // 0 to 12

} // namespace

Read readHeaders(const uint8_t* bytes, size_t length, StreamHeader& out, std::string& error)
{
    if (length > MAX_STREAM_BYTES)
        length = MAX_STREAM_BYTES;
    StreamHeader h;

    // ---- The video header: one intra frame.
    BitReader v(bytes, length, 0);
    const uint64_t frames = v.read(12), intras = v.read(12), pFrames = v.read(12);
    const size_t videoBytes = size_t(v.read(16));
    if (v.ranShort())
        return Read::NeedMore;
    if (frames != 1 || intras != 1 || pFrames != 0)
        return error = "not a single picture", Read::Bad;
    const uint64_t intraPos = v.read(12);
    if (v.ranShort())
        return Read::NeedMore;
    if (intraPos != 0)
        return error = "not a single picture", Read::Bad;
    size_t at = 0;
    if (!headerLength(v, 0, videoBytes, at, error))
        return Read::Bad;

    // ---- The frame header: I, RGB, 8 bits.
    BitReader f(bytes, length, at);
    const uint64_t display = f.read(12), type = f.read(2), dataType = f.read(2), depth = f.read(4);
    const size_t frameBytes = size_t(f.read(16));
    if (f.ranShort())
        return Read::NeedMore;
    if (display != 0 || type != 0 || dataType != 0 || depth != 0)
        return error = "not an 8-bit RGB intra picture", Read::Bad;
    if (!headerLength(f, at, frameBytes, at, error))
        return Read::Bad;

    // ---- The Cool-chic header.
    BitReader c(bytes, length, at);
    Architecture& a = h.arch;
    a.synthesisStabiliser = c.bit() != 0;
    const int layers = int(c.read(3));
    a.upsKernel = int(c.read(4));
    a.preconcatKernel = int(c.read(4));
    a.ifceOutputs = int(c.read(5));
    a.spatialContext = int(c.read(6));
    a.armStabiliser = c.bit() != 0;
    a.armHiddenLayers = int(c.read(3));
    a.height = int(c.read(14));
    a.width = int(c.read(14));
    a.latentLow = int(c.read(4));
    a.latentHigh = int(c.read(4));
    const int gridCount = int(c.read(5));
    const bool hyper = c.bit() != 0;
    const bool commonRandomness = c.bit() != 0;
    const int finalUpsampling = int(c.read(2));
    int stepIndex[MODULES][2], orderIndex[MODULES][2];
    for (int m = 0; m < MODULES; m++)
        for (int wb = 0; wb < 2; wb++)
            stepIndex[m][wb] = int(c.read(5));
    for (int m = 0; m < MODULES; m++)
        for (int wb = 0; wb < 2; wb++)
            orderIndex[m][wb] = int(c.read(4));
    h.weightBytes = size_t(c.read(14));
    h.weightPadBits = int(c.read(3));
    h.latentBytes = size_t(c.read(28));
    const size_t coolchicBytes = size_t(c.read(16));
    if (a.ifceOutputs > 0)
    {
        a.ifceLow = int(c.read(4));
        a.ifceHigh = int(c.read(4));
    }
    if (hyper)
    {
        a.hyperLow = int(c.read(4));
        a.hyperHigh = int(c.read(4));
    }
    else
    {
        a.hyperLow = 1;
        a.hyperHigh = 0;
    }
    a.synthesis.clear();
    for (int i = 0; i < layers; i++)
    {
        SynthesisLayer l;
        l.outputs = int(c.read(7));
        l.kernel = int(c.read(4));
        l.residual = c.bit() != 0;
        l.relu = c.bit() != 0;
        a.synthesis.push_back(l);
    }
    if (c.ranShort())
        return Read::NeedMore;

    if (commonRandomness)
        return error = "common randomness isn't supported", Read::Bad;
    if (finalUpsampling > 2)
        return error = "unknown final upsampling", Read::Bad;
    if (!valid(a, error))
        return Read::Bad;
    if (size_t(gridCount) != grids(a).size())
        return error = "grid count doesn't match", Read::Bad;
    for (int m = 0; m < MODULES; m++)
    {
        if (stepIndex[m][0] >= WEIGHT_STEPS[m].count || stepIndex[m][1] >= BIAS_STEPS[m].count ||
            orderIndex[m][0] >= EXP_GOLOMB_ORDERS || orderIndex[m][1] >= EXP_GOLOMB_ORDERS)
            return error = "quantisation index out of range", Read::Bad;
        h.net.weightShift[m] = WEIGHT_STEPS[m].first + stepIndex[m][0];
        h.net.biasShift[m] = BIAS_STEPS[m].first + stepIndex[m][1];
        h.net.weightExpGolomb[m] = orderIndex[m][0];
        h.net.biasExpGolomb[m] = orderIndex[m][1];
    }
    if (!headerLength(c, at, coolchicBytes, at, error))
        return Read::Bad;

    h.weightsStart = at;
    h.latentStart = at + h.weightBytes;
    if (h.latentStart + h.latentBytes > MAX_STREAM_BYTES)
        return error = "stream too long", Read::Bad;
    out = h;
    return Read::Done;
}

bool writeHeaders(const StreamHeader& h, std::vector<uint8_t>& out, std::string& error)
{
    const Architecture& a = h.arch;
    if (!valid(a, error))
        return false;
    int stepIndex[MODULES][2];
    for (int m = 0; m < MODULES; m++)
    {
        stepIndex[m][0] = h.net.weightShift[m] - WEIGHT_STEPS[m].first;
        stepIndex[m][1] = h.net.biasShift[m] - BIAS_STEPS[m].first;
        if (stepIndex[m][0] < 0 || stepIndex[m][0] >= WEIGHT_STEPS[m].count ||
            stepIndex[m][1] < 0 || stepIndex[m][1] >= BIAS_STEPS[m].count ||
            h.net.weightExpGolomb[m] < 0 || h.net.weightExpGolomb[m] >= EXP_GOLOMB_ORDERS ||
            h.net.biasExpGolomb[m] < 0 || h.net.biasExpGolomb[m] >= EXP_GOLOMB_ORDERS)
            return error = "quantisation out of range", false;
    }
    if (!fits(h.weightBytes, 14) || h.weightPadBits < 0 || h.weightPadBits > 7 ||
        !fits(h.latentBytes, 28))
        return error = "lengths too big for the header", false;
    for (const SynthesisLayer& l : a.synthesis)
        if (!fits(uint64_t(l.outputs), 7) || !fits(uint64_t(l.kernel), 4))
            return error = "synthesis layer too big for the header", false;
    if (!fits(uint64_t(a.upsKernel), 4) || !fits(uint64_t(a.preconcatKernel), 4) ||
        !fits(uint64_t(a.ifceOutputs), 5) || !fits(uint64_t(a.spatialContext), 6) ||
        !fits(uint64_t(a.armHiddenLayers), 3) || !fits(uint64_t(a.latentHigh), 4) ||
        (a.ifceOutputs > 0 && !fits(uint64_t(a.ifceHigh), 4)) ||
        (a.hasHyperlatents() && !fits(uint64_t(a.hyperHigh), 4)))
        return error = "shape too big for the header", false;

    // Each header's bits, then its length in bytes, which sits at the end of
    // its fixed part but counts the whole header.
    std::vector<uint8_t> all;
    {
        // The video header: one frame, an intra at 0.
        const uint64_t bits = 12 + 12 + 12 + 16 + 12;
        BitWriter w;
        w.write(1, 12);
        w.write(1, 12);
        w.write(0, 12);
        w.write((bits + 7) / 8, 16);
        w.write(0, 12);
        all.insert(all.end(), w.bytes().begin(), w.bytes().end());
    }
    {
        // The frame header: display 0, I, RGB, 8 bits.
        const uint64_t bits = 12 + 2 + 2 + 4 + 16;
        BitWriter w;
        w.write(0, 12);
        w.write(0, 2);
        w.write(0, 2);
        w.write(0, 4);
        w.write((bits + 7) / 8, 16);
        all.insert(all.end(), w.bytes().begin(), w.bytes().end());
    }
    {
        const uint64_t bits = 1 + 3 + 4 + 4 + 5 + 6 + 1 + 3 + 28 + 8 + 5 + 1 + 1 + 2 + 8 * 5 +
                              8 * 4 + 14 + 3 + 28 + 16 + (a.ifceOutputs > 0 ? 8 : 0) +
                              (a.hasHyperlatents() ? 8 : 0) + 13 * uint64_t(a.synthesis.size());
        BitWriter w;
        w.write(a.synthesisStabiliser ? 1 : 0, 1);
        w.write(a.synthesis.size(), 3);
        w.write(uint64_t(a.upsKernel), 4);
        w.write(uint64_t(a.preconcatKernel), 4);
        w.write(uint64_t(a.ifceOutputs), 5);
        w.write(uint64_t(a.spatialContext), 6);
        w.write(a.armStabiliser ? 1 : 0, 1);
        w.write(uint64_t(a.armHiddenLayers), 3);
        w.write(uint64_t(a.height), 14);
        w.write(uint64_t(a.width), 14);
        w.write(uint64_t(a.latentLow), 4);
        w.write(uint64_t(a.latentHigh), 4);
        w.write(grids(a).size(), 5);
        w.write(a.hasHyperlatents() ? 1 : 0, 1);
        w.write(0, 1);                  // no common randomness
        w.write(2, 2);                  // final upsampling: bicubic, as Cool-chic's residue
        for (int m = 0; m < MODULES; m++)
            for (int wb = 0; wb < 2; wb++)
                w.write(uint64_t(stepIndex[m][wb]), 5);
        for (int m = 0; m < MODULES; m++)
        {
            w.write(uint64_t(h.net.weightExpGolomb[m]), 4);
            w.write(uint64_t(h.net.biasExpGolomb[m]), 4);
        }
        w.write(h.weightBytes, 14);
        w.write(uint64_t(h.weightPadBits), 3);
        w.write(h.latentBytes, 28);
        w.write((bits + 7) / 8, 16);
        if (a.ifceOutputs > 0)
        {
            w.write(uint64_t(a.ifceLow), 4);
            w.write(uint64_t(a.ifceHigh), 4);
        }
        if (a.hasHyperlatents())
        {
            w.write(uint64_t(a.hyperLow), 4);
            w.write(uint64_t(a.hyperHigh), 4);
        }
        for (const SynthesisLayer& l : a.synthesis)
        {
            w.write(uint64_t(l.outputs), 7);
            w.write(uint64_t(l.kernel), 4);
            w.write(l.residual ? 1 : 0, 1);
            w.write(l.relu ? 1 : 0, 1);
        }
        if (w.bits() != bits)
            return error = "header miscounted", false;
        all.insert(all.end(), w.bytes().begin(), w.bytes().end());
    }
    out = std::move(all);
    return true;
}

bool readWeights(const uint8_t* bytes, const StreamHeader& h, QuantisedNetwork& net,
                 std::string& error)
{
    BitReader r(bytes, h.weightBytes, 0);
    r.read(h.weightPadBits);
    net = h.net;
    for (int m = 0; m < MODULES; m++)
    {
        // A module Cool-chic doesn't make (no IFCE) sends nothing.
        if (m == IFCE && h.arch.ifceOutputs == 0)
        {
            net.weights[m].clear();
            net.biases[m].clear();
            continue;
        }
        for (int wb = 0; wb < 2; wb++)
        {
            const size_t count = parameterCount(h.arch, Module(m), wb == 1);
            const int order = wb == 0 ? h.net.weightExpGolomb[m] : h.net.biasExpGolomb[m];
            std::vector<int32_t>& list = wb == 0 ? net.weights[m] : net.biases[m];
            list.clear();
            list.reserve(count);
            for (size_t i = 0; i < count; i++)
            {
                // Exp-Golomb of order k: n zeros, then n + 1 bits of
                // quotient + 1, then k bits of remainder.
                int zeros = 0;
                while (r.bit() == 0)
                {
                    if (r.ranShort() || ++zeros > 32)
                        return error = "bad weight code", false;
                }
                const uint64_t quotient = ((uint64_t(1) << zeros) | r.read(zeros)) - 1;
                const uint64_t value = (quotient << order) | r.read(order);
                if (r.ranShort())
                    return error = "weights cut short", false;
                // Odd codes are positive, even ones zero or negative.
                const int64_t signedValue = (value & 1) ? int64_t((value + 1) / 2)
                                                        : -int64_t(value / 2);
                if (signedValue > INT32_MAX || signedValue < -int64_t(INT32_MAX))
                    return error = "weight out of range", false;
                list.push_back(int32_t(signedValue));
            }
        }
    }
    return true;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
