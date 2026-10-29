// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/header/ (header.py, element.py) and
// coolchic/bitstream/neuralnet/ (neuralnet.py, expgolomb.py). The licence
// is LICENSE.txt beside this file.
//
// The headers and the weights of a stream (docs/PICTURE_CODEC.md, "The
// stream"), read with every count and length checked.
#ifndef RAGTIME_IMAGE_COOLCHIC_STREAM_H
#define RAGTIME_IMAGE_COOLCHIC_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// What the three headers say.
struct StreamHeader
{
    Architecture arch;
    QuantisedNetwork net;           // the shifts and exp-Golomb orders; no weights yet
    size_t weightsStart = 0;        // where the weights start
    size_t weightBytes = 0;         // nn_n_bytes
    int weightPadBits = 0;          // nn_n_bit_pad, before the first weight
    size_t latentStart = 0;         // where the latent part starts
    size_t latentBytes = 0;         // n_bytes_latent
};

enum class Read
{
    Done,
    NeedMore,   // the bytes so far end inside the headers
    Bad         // not a stream we take (error says why)
};

// The video, frame and Cool-chic headers from the first length bytes.
Read readHeaders(const uint8_t* bytes, size_t length, StreamHeader& out, std::string& error);

// The three headers as Cool-chic writes them, from h's arch, net's shifts
// and orders, weightBytes, weightPadBits and latentBytes (the positions
// are ignored). False, with the reason, if they can't be written: a shape
// outside the caps, or a field too big for its bits.
bool writeHeaders(const StreamHeader& h, std::vector<uint8_t>& out, std::string& error);

// The weights, exp-Golomb coded (bytes is the weight part, weightBytes
// long), into net's lists in the order QuantisedNetwork says.
bool readWeights(const uint8_t* bytes, const StreamHeader& header, QuantisedNetwork& net,
                 std::string& error);

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
