// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). The headers and the weights are ported to C++ for
// Ragtime from Cool-chic 5.0.1 (coolchic/bitstream/header/,
// coolchic/bitstream/neuralnet/). Authors: see Cool-chic's CONTRIBUTORS.md.
// The latent coding (the range encoder and the grid table) is Ragtime's own,
// from docs/PICTURE_CODEC.md.
//
// Writing a fitted picture as a Ragtime Cool-chic stream: Cool-chic's video,
// frame and Cool-chic headers, its exp-Golomb weights, then the latents,
// one range-coded run per grid, coarsest first, behind a table of their
// lengths.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_WRITE_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_WRITE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "FitModel.h"
#include "FitNet.h"
#include "FitQuant.h"
#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// Bits, most significant first, as Cool-chic's headers string them.
class BitWriter
{
public:
    void put(uint64_t value, int bits);
    size_t size() const { return bits_; }
    // Padded with zero bits to a whole byte.
    std::vector<uint8_t> bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bits_ = 0;
};

// expgolomb.py's encode_exp_golomb(): each value with its own order, the
// zero bits that pad the message to whole bytes in front.
std::vector<uint8_t> expGolombEncode(const std::vector<int32_t>& values,
                                     const std::vector<int>& orders, int& padBits);

// The range encoder of docs/PICTURE_CODEC.md for one grid's run: LZMA's,
// with a 64-bit range and the carry out of low kept as a 65th bit.
class RangeEncoder
{
public:
    // A latent value (clamped to [-64, 63]) with the ARM's mu and scale
    // indices (clamped to their ranges).
    void encode(int value, int mu, int scale);
    // The run: flushed, its first byte and trailing zeros dropped.
    std::vector<uint8_t> finish();

private:
    void shiftLow();

    void add(uint64_t v);

    uint64_t low_ = 0;
    int carry_ = 0;         // bit 64 of low
    uint64_t range_ = ~(uint64_t)0;
    uint8_t cache_ = 0;
    uint64_t cacheSize_ = 1;
    std::vector<uint8_t> out_;
};

// The latent part: the run lengths as LEB128 varints, then the runs.
std::vector<uint8_t> packGrids(const std::vector<std::vector<uint8_t>>& runs);

// The networks as sent, from params quantised by q.
QuantisedNetwork sentNetwork(const std::vector<float>& params, const Quantiser& q);

// The whole stream for params quantised by q: false, with the reason, if
// the model can't be written (a shape outside the decoder's caps). net is
// used for the latents' hard rounding; clamped tells how many latents fell
// outside [-64, 63] and were clamped.
bool writeStream(const Model& model, const std::vector<float>& params, Net& net,
                 const Quantiser& q, std::vector<uint8_t>& out, std::string& error,
                 size_t* clamped = nullptr);

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
