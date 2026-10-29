// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/component/core/upsampling.py and synthesis.py. The licence is
// LICENSE.txt beside this file.
//
// The upsampling and the synthesis, in float: nothing is decoded from the
// pixels, so a pixel a hair different on another machine does no harm.
#ifndef RAGTIME_IMAGE_COOLCHIC_UPSAMPLING_H
#define RAGTIME_IMAGE_COOLCHIC_UPSAMPLING_H

#include <cstddef>
#include <string>
#include <vector>

#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// One channel of floats, row by row.
struct Plane
{
    int height = 0;
    int width = 0;
    std::vector<float> v;

    static Plane zeros(int height, int width)
    {
        Plane p;
        p.height = height;
        p.width = width;
        p.v.assign(size_t(height) * size_t(width), 0.0f);
        return p;
    }
    float& at(int y, int x) { return v[size_t(y) * size_t(width) + size_t(x)]; }
    float at(int y, int x) const { return v[size_t(y) * size_t(width) + size_t(x)]; }
};

// The upsampling's filters, whole: each transposed convolution's even
// kernel and each pre-concatenation filter's odd one, symmetric.
struct UpsamplingWeights
{
    std::vector<std::vector<float>> transposed;
    std::vector<std::vector<float>> preconcat;
};

// The synthesis' layers, the stabiliser and the output transform.
struct SynthesisWeights
{
    struct Conv
    {
        int inputs = 0;
        int outputs = 0;
        int kernel = 1;
        bool residual = false;
        bool relu = false;
        std::vector<float> weight;      // [outputs][inputs][kernel][kernel]
        std::vector<float> bias;        // [outputs]
    };
    std::vector<Conv> layers;
    Conv stabiliser;                    // no weights if there is none
    Conv output;                        // 1x1, channels to channels
};

// The weights as floats, integer x 2^shift. False if they don't fit.
bool upsamplingWeights(const Architecture& arch, const QuantisedNetwork& net,
                       UpsamplingWeights& out, std::string& error);
bool synthesisWeights(const Architecture& arch, const QuantisedNetwork& net,
                      SynthesisWeights& out, std::string& error);

// Upsampling.forward: the synthesis grids, finest first, to one plane each
// at the finest grid's size, in the same order.
std::vector<Plane> upsample(const UpsamplingWeights& w, const std::vector<Plane>& latents);

// Synthesis.forward: the upsampled planes to the picture's channels.
std::vector<Plane> synthesise(const SynthesisWeights& w, const std::vector<Plane>& input);

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
