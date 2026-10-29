// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/component/core/coolchic.py (CoolChicEncoderParameter),
// coolchic/utils/parsecli.py and cfg/dec/intra/*.cfg. The licence is
// LICENSE.txt beside this file.
//
// The network's shape: the presets, the caps and the grids.

#include "ragtime/image/CoolChic.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

SynthesisLayer layer(int outputs, int kernel, bool residual, bool relu)
{
    SynthesisLayer l;
    l.outputs = outputs;
    l.kernel = kernel;
    l.residual = residual;
    l.relu = relu;
    return l;
}

// ceil(side / 2^down), as Cool-chic sizes its grids.
int gridSide(int side, int down)
{
    const int64_t step = int64_t(1) << down;
    return int((side + step - 1) / step);
}

// Cool-chic's downsampling ratio for a grid: ceil(log2(height / grid height)).
int ratio(int height, int gridHeight)
{
    int r = 0;
    while (r < 31 && (int64_t(gridHeight) << r) < height)
        r++;
    return r;
}

} // namespace

bool preset(const std::string& name, int height, int width, Architecture& out)
{
    Architecture a;
    a.height = height;
    a.width = width;
    // "auto": down to 1/64 under a million pixels, 1/128 under three, else
    // 1/256, with hyperlatents from 1/16 down to the same.
    const int64_t pixels = int64_t(height) * width;
    const int coarsest = pixels < 1000000 ? 6 : pixels < 3000000 ? 7 : 8;
    a.latentLow = 0;
    a.latentHigh = coarsest;
    a.hyperLow = 4;
    a.hyperHigh = coarsest;
    a.armHiddenLayers = 2;
    a.armStabiliser = true;
    a.ifceLow = 0;
    a.ifceHigh = 2;
    a.upsKernel = 8;
    a.preconcatKernel = 7;
    a.synthesisStabiliser = true;

    int armContext = 0, ifce = 0, wide = 0;
    if (name == "lop")
        armContext = 6, ifce = 2, wide = 8;
    else if (name == "mop")
        armContext = 10, ifce = 4, wide = 16;
    else if (name == "hop")
        armContext = 14, ifce = 6, wide = 48;
    else if (name == "vhop")
        armContext = 20, ifce = 6, wide = 64;
    else
        return false;
    a.spatialContext = armContext;
    a.ifceOutputs = ifce;
    a.synthesis.push_back(layer(wide, 1, false, true));
    a.synthesis.push_back(layer(3, 1, false, false));
    if (name != "lop")
        a.synthesis.push_back(layer(3, 3, true, true));
    a.synthesis.push_back(layer(3, 3, true, false));
    out = a;
    return true;
}

bool valid(const Architecture& a, std::string& error)
{
    if (a.height < 1 || a.width < 1 || a.height > MAX_SIDE || a.width > MAX_SIDE ||
        int64_t(a.height) * a.width > MAX_PIXELS)
        return error = "picture size out of range", false;
    // A first grid smaller than the picture would need a final upsampling,
    // which Ragtime pictures don't use.
    if (a.latentLow != 0 || a.latentHigh < 0 || a.latentHigh > MAX_DOWNSAMPLING)
        return error = "latent grids out of range", false;
    if (a.hasHyperlatents() && (a.hyperLow < 0 || a.hyperHigh > MAX_DOWNSAMPLING))
        return error = "hyperlatent grids out of range", false;
    if (a.spatialContext < 0 || a.spatialContext > MAX_SPATIAL_CONTEXT ||
        a.armHiddenLayers < 0 || a.armHiddenLayers > MAX_ARM_HIDDEN_LAYERS)
        return error = "ARM out of range", false;
    if (a.ifceOutputs < 0 || a.ifceOutputs > MAX_IFCE_OUTPUTS ||
        (a.ifceOutputs > 0 && (a.ifceLow < 0 || a.ifceLow > a.ifceHigh ||
                               a.ifceHigh > MAX_DOWNSAMPLING)))
        return error = "IFCE out of range", false;
    if (a.spatialContext + a.ifceOutputs < 1)
        return error = "the ARM has no context", false;
    if (a.upsKernel < 4 || a.upsKernel > MAX_UPS_KERNEL || a.upsKernel % 2 != 0 ||
        a.preconcatKernel < 1 || a.preconcatKernel > MAX_PRECONCAT_KERNEL ||
        a.preconcatKernel % 2 != 1)
        return error = "upsampling kernels out of range", false;
    if (a.synthesis.empty() || a.synthesis.size() > size_t(MAX_SYNTHESIS_LAYERS))
        return error = "synthesis layers out of range", false;
    int inputs = a.synthesisInputs();
    for (const SynthesisLayer& l : a.synthesis)
    {
        if (l.outputs < 1 || l.outputs > MAX_SYNTHESIS_WIDTH || l.kernel < 1 ||
            l.kernel > MAX_SYNTHESIS_KERNEL || l.kernel % 2 != 1)
            return error = "synthesis layer out of range", false;
        if (l.residual && l.outputs != inputs)
            return error = "residual synthesis layer changes width", false;
        inputs = l.outputs;
    }
    if (a.channels() != 3)
        return error = "not an RGB picture", false;
    if (grids(a).size() > size_t(MAX_GRIDS))
        return error = "too many grids", false;
    return true;
}

std::vector<Grid> grids(const Architecture& a)
{
    std::vector<Grid> out;
    if (a.height < 1 || a.width < 1 || a.latentLow < 0 || a.latentHigh > MAX_DOWNSAMPLING ||
        (a.hasHyperlatents() && (a.hyperLow < 0 || a.hyperHigh > MAX_DOWNSAMPLING)))
        return out;
    int low = a.latentLow, high = a.latentHigh;
    if (a.hasHyperlatents())
    {
        low = std::min(low, a.hyperLow);
        high = std::max(high, a.hyperHigh);
    }
    for (int i = low; i <= high; i++)
    {
        Grid g;
        g.height = gridSide(a.height, i);
        g.width = gridSide(a.width, i);
        g.downsampling = i;
        if (a.latentLow <= i && i <= a.latentHigh)
            out.push_back(g);
        if (a.hasHyperlatents() && a.hyperLow <= i && i <= a.hyperHigh)
        {
            g.hyper = true;
            out.push_back(g);
        }
    }
    // Cool-chic takes the IFCE's grids by the ratio of heights, rounded up.
    const int n = int(out.size());
    for (int i = 0; i < n; i++)
    {
        const int r = ratio(a.height, out[size_t(i)].height);
        if (a.ifceOutputs > 0 && a.ifceLow <= r && r <= a.ifceHigh)
            out[size_t(i)].ifceInputs = std::max(n - 1 - i, 1);
    }
    return out;
}

size_t parameterCount(const Architecture& a, Module module, bool biases)
{
    size_t n = 0;
    switch (module)
    {
    case ARM:
    {
        const size_t dim = size_t(a.spatialContext + a.ifceOutputs);
        const size_t hidden = size_t(a.armHiddenLayers);
        if (biases)
            n = hidden * dim + 2 + (a.armStabiliser ? 2 : 0);
        else
            n = hidden * dim * dim + dim * 2 + (a.armStabiliser ? dim * 2 : 0);
        break;
    }
    case IFCE:
        if (a.ifceOutputs > 0)
            for (const Grid& g : grids(a))
                if (g.ifceInputs > 0)
                    n += size_t(a.ifceOutputs) * (biases ? 1 : size_t(g.ifceInputs));
        break;
    case UPSAMPLING:
    {
        const size_t steps = size_t(a.latentHigh);
        if (biases)
            n = 2 * steps;
        else
            n = steps * size_t((a.upsKernel + 1) / 2) + steps * size_t((a.preconcatKernel + 1) / 2);
        break;
    }
    case SYNTHESIS:
    {
        const size_t c = size_t(a.channels());
        size_t inputs = size_t(a.synthesisInputs());
        n = biases ? c : c * c;
        if (a.synthesisStabiliser)
            n += biases ? c : c * inputs;
        for (const SynthesisLayer& l : a.synthesis)
        {
            const size_t out = size_t(l.outputs), k = size_t(l.kernel);
            n += biases ? out : out * inputs * k * k;
            inputs = out;
        }
        break;
    }
    default:
        break;
    }
    return n;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
