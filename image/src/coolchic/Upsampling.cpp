// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/component/core/upsampling.py and synthesis.py. The licence is
// LICENSE.txt beside this file.

#include "Upsampling.h"

#include <algorithm>
#include <cmath>
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

// A symmetric kernel of size k from its first (k + 1) / 2 taps:
// a b c d -> a b c d d c b a (k = 8) or a b c d c b a (k = 7).
std::vector<float> symmetric(const int32_t* half, int k, int shift)
{
    const int n = (k + 1) / 2;
    std::vector<float> kernel(size_t(k), 0.0f);
    for (int i = 0; i < n; i++)
    {
        const float v = std::ldexp(float(half[i]), shift);
        kernel[size_t(i)] = v;
        kernel[size_t(k - 1 - i)] = v;
    }
    return kernel;
}

// One row (or column) through a stride-2 transposed convolution with
// replicate padding, cropped as UpsamplingSeparableSymmetricConvTranspose2d
// crops it: n samples in, 2n out.
void transposedRow(const float* in, size_t inStride, int n, const std::vector<float>& w,
                   float* out, size_t outStride)
{
    const int k = int(w.size());
    const int pad = k / 2;
    const int crop = 2 * pad - 1 + k / 2;
    for (int t = 0; t < 2 * n; t++)
    {
        const int o = t + crop;
        float sum = 0.0f;
        // o = 2 i + j for padded input i and tap j.
        for (int j = o % 2; j < k; j += 2)
        {
            const int i = (o - j) / 2;
            const int src = std::min(std::max(i - pad, 0), n - 1);
            sum += in[size_t(src) * inStride] * w[size_t(j)];
        }
        out[size_t(t) * outStride] = sum;
    }
}

// Upsampled 2x by the separable transposed convolution, then cropped to
// height x width.
Plane transposed(const Plane& x, const std::vector<float>& w, int height, int width)
{
    Plane wide = Plane::zeros(x.height, 2 * x.width);
    for (int y = 0; y < x.height; y++)
        transposedRow(&x.v[size_t(y) * size_t(x.width)], 1, x.width, w,
                      &wide.v[size_t(y) * size_t(wide.width)], 1);
    Plane tall = Plane::zeros(2 * x.height, wide.width);
    for (int c = 0; c < wide.width; c++)
        transposedRow(&wide.v[size_t(c)], size_t(wide.width), x.height, w, &tall.v[size_t(c)],
                      size_t(tall.width));
    Plane out = Plane::zeros(height, width);
    for (int y = 0; y < height; y++)
        for (int c = 0; c < width; c++)
            out.at(y, c) = tall.at(y, c);
    return out;
}

// The pre-concatenation filter: a separable convolution with zero padding,
// plus its input.
Plane preconcat(const Plane& x, const std::vector<float>& w)
{
    const int k = int(w.size());
    const int pad = k / 2;
    Plane across = Plane::zeros(x.height, x.width);
    for (int y = 0; y < x.height; y++)
        for (int c = 0; c < x.width; c++)
        {
            float sum = 0.0f;
            for (int j = 0; j < k; j++)
            {
                const int s = c + j - pad;
                if (s >= 0 && s < x.width)
                    sum += x.at(y, s) * w[size_t(j)];
            }
            across.at(y, c) = sum;
        }
    Plane out = Plane::zeros(x.height, x.width);
    for (int y = 0; y < x.height; y++)
        for (int c = 0; c < x.width; c++)
        {
            float sum = 0.0f;
            for (int j = 0; j < k; j++)
            {
                const int s = y + j - pad;
                if (s >= 0 && s < x.height)
                    sum += across.at(s, c) * w[size_t(j)];
            }
            out.at(y, c) = sum + x.at(y, c);
        }
    return out;
}

SynthesisWeights::Conv conv(const int32_t*& w, const int32_t*& b, int inputs, int outputs,
                            int kernel, bool residual, bool relu, int weightShift, int biasShift)
{
    SynthesisWeights::Conv c;
    c.inputs = inputs;
    c.outputs = outputs;
    c.kernel = kernel;
    c.residual = residual;
    c.relu = relu;
    const size_t n = size_t(outputs) * size_t(inputs) * size_t(kernel) * size_t(kernel);
    c.weight.resize(n);
    for (size_t i = 0; i < n; i++)
        c.weight[i] = std::ldexp(float(w[i]), weightShift);
    c.bias.resize(size_t(outputs));
    for (int o = 0; o < outputs; o++)
        c.bias[size_t(o)] = std::ldexp(float(b[o]), biasShift);
    w += n;
    b += outputs;
    return c;
}

// SynthesisConv2d.forward: replicate padding, then the residual.
std::vector<Plane> run(const SynthesisWeights::Conv& c, const std::vector<Plane>& x)
{
    const int h = x.empty() ? 0 : x[0].height;
    const int wd = x.empty() ? 0 : x[0].width;
    const int k = c.kernel, pad = k / 2;
    std::vector<Plane> out(size_t(c.outputs));
    for (int o = 0; o < c.outputs; o++)
    {
        Plane p = Plane::zeros(h, wd);
        std::fill(p.v.begin(), p.v.end(), c.bias[size_t(o)]);
        for (int i = 0; i < c.inputs; i++)
        {
            const Plane& in = x[size_t(i)];
            const float* kw = &c.weight[(size_t(o) * size_t(c.inputs) + size_t(i)) * size_t(k * k)];
            if (k == 1)
            {
                const float wv = kw[0];
                for (size_t n = 0; n < p.v.size(); n++)
                    p.v[n] += in.v[n] * wv;
                continue;
            }
            for (int dy = 0; dy < k; dy++)
                for (int dx = 0; dx < k; dx++)
                {
                    const float wv = kw[dy * k + dx];
                    for (int y = 0; y < h; y++)
                    {
                        const int sy = std::min(std::max(y + dy - pad, 0), h - 1);
                        const float* src = &in.v[size_t(sy) * size_t(wd)];
                        float* dst = &p.v[size_t(y) * size_t(wd)];
                        for (int cx = 0; cx < wd; cx++)
                        {
                            const int sx = std::min(std::max(cx + dx - pad, 0), wd - 1);
                            dst[cx] += src[sx] * wv;
                        }
                    }
                }
        }
        if (c.residual)
            for (size_t n = 0; n < p.v.size(); n++)
                p.v[n] += x[size_t(o)].v[n];
        if (c.relu)
            for (float& v : p.v)
                v = std::max(v, 0.0f);
        out[size_t(o)] = std::move(p);
    }
    return out;
}

} // namespace

bool upsamplingWeights(const Architecture& arch, const QuantisedNetwork& net,
                       UpsamplingWeights& out, std::string& error)
{
    const std::vector<int32_t>& w = net.weights[UPSAMPLING];
    if (w.size() != parameterCount(arch, UPSAMPLING, false) ||
        net.biases[UPSAMPLING].size() != parameterCount(arch, UPSAMPLING, true))
        return error = "upsampling weights don't fit", false;
    if (net.weightShift[UPSAMPLING] < -12 || net.weightShift[UPSAMPLING] > 0)
        return error = "upsampling quantisation step out of range", false;
    UpsamplingWeights u;
    const int steps = arch.latentHigh;
    const int32_t* p = w.data();
    for (int i = 0; i < steps; i++, p += (arch.upsKernel + 1) / 2)
        u.transposed.push_back(symmetric(p, arch.upsKernel, net.weightShift[UPSAMPLING]));
    for (int i = 0; i < steps; i++, p += (arch.preconcatKernel + 1) / 2)
        u.preconcat.push_back(symmetric(p, arch.preconcatKernel, net.weightShift[UPSAMPLING]));
    out = std::move(u);
    return true;
}

bool synthesisWeights(const Architecture& arch, const QuantisedNetwork& net,
                      SynthesisWeights& out, std::string& error)
{
    const std::vector<int32_t>& w = net.weights[SYNTHESIS];
    const std::vector<int32_t>& b = net.biases[SYNTHESIS];
    if (w.size() != parameterCount(arch, SYNTHESIS, false) ||
        b.size() != parameterCount(arch, SYNTHESIS, true))
        return error = "synthesis weights don't fit", false;
    const int ws = net.weightShift[SYNTHESIS], bs = net.biasShift[SYNTHESIS];
    if (ws < -12 || ws > 0 || bs < -24 || bs > 0)
        return error = "synthesis quantisation step out of range", false;

    SynthesisWeights s;
    const int32_t* pw = w.data();
    const int32_t* pb = b.data();
    const int channels = arch.channels();
    int inputs = arch.synthesisInputs();
    s.output = conv(pw, pb, channels, channels, 1, false, false, ws, bs);
    if (arch.synthesisStabiliser)
        s.stabiliser = conv(pw, pb, inputs, channels, 1, false, false, ws, bs);
    for (const SynthesisLayer& l : arch.synthesis)
    {
        s.layers.push_back(conv(pw, pb, inputs, l.outputs, l.kernel, l.residual, l.relu, ws, bs));
        inputs = l.outputs;
    }
    out = std::move(s);
    return true;
}

std::vector<Plane> upsample(const UpsamplingWeights& w, const std::vector<Plane>& latents)
{
    if (latents.empty())
        return {};
    // From the coarsest: upsample everything so far to the next grid's
    // size, filter that grid, and put it in front.
    std::vector<Plane> stack{latents.back()};
    const size_t n = latents.size();
    for (size_t idx = 0; idx + 1 < n; idx++)
    {
        const Plane& target = latents[n - 2 - idx];
        std::vector<Plane> next;
        next.reserve(stack.size() + 1);
        next.push_back(preconcat(target, w.preconcat[idx % w.preconcat.size()]));
        const std::vector<float>& k = w.transposed[idx % w.transposed.size()];
        for (const Plane& p : stack)
            next.push_back(transposed(p, k, target.height, target.width));
        stack = std::move(next);
    }
    return stack;
}

std::vector<Plane> synthesise(const SynthesisWeights& w, const std::vector<Plane>& input)
{
    std::vector<Plane> x = input;
    for (const SynthesisWeights::Conv& c : w.layers)
        x = run(c, x);
    if (!w.stabiliser.weight.empty())
    {
        std::vector<Plane> s = run(w.stabiliser, input);
        for (size_t c = 0; c < x.size() && c < s.size(); c++)
            for (size_t n = 0; n < x[c].v.size(); n++)
                x[c].v[n] += s[c].v[n];
    }
    return run(w.output, x);
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
