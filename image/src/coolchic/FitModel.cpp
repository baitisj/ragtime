// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1: the
// architecture and initialisation of coolchic/component/core/ (coolchic.py,
// arm.py, upsampling.py, synthesis.py) and cfg/dec/intra/. Authors: see
// Cool-chic's CONTRIBUTORS.md.
#include "FitModel.h"

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

constexpr int MAX_SIDE = 4096;
constexpr int MAX_CONTEXT = 40;

std::vector<SynLayer> layers(std::initializer_list<SynLayer> list)
{
    return std::vector<SynLayer>(list);
}

// arm.py's get_priority_order(): the 9x9 mask's first 40 pixels (those
// before the one coded), each with its priority.
const int PRIORITY[40] = {
    38, 35, 30, 25, 23, 31, 36, 37, 39,
    33, 28, 21, 20, 6, 15, 22, 29, 34,
    32, 18, 12, 10, 5, 9, 14, 19, 27,
    24, 13, 8, 2, 1, 3, 11, 17, 26,
    16, 7, 4, 0,
};

int ceilShift(int x, int level)
{
    return (x + (1 << level) - 1) >> level;
}

} // namespace

bool archByName(const std::string& name, Arch& out)
{
    Arch a;
    a.name = name;
    // Every intra config: latents auto, hyperlatents auto, IFCE at 0-2,
    // upsampling 8 and 7, stabilisers on.
    if (name == "lop")
    {
        a.armContext = 6;
        a.ifceOut = 2;
        a.synthesis = layers({{8, 1, false, true}, {3, 1, false, false}, {3, 3, true, false}});
    }
    else if (name == "mop")
    {
        a.armContext = 10;
        a.ifceOut = 4;
        a.synthesis = layers({{16, 1, false, true}, {3, 1, false, false}, {3, 3, true, true},
                              {3, 3, true, false}});
    }
    else if (name == "hop")
    {
        a.armContext = 14;
        a.ifceOut = 6;
        a.synthesis = layers({{48, 1, false, true}, {3, 1, false, false}, {3, 3, true, true},
                              {3, 3, true, false}});
    }
    else if (name == "vhop")
    {
        a.armContext = 20;
        a.ifceOut = 6;
        a.synthesis = layers({{64, 1, false, true}, {3, 1, false, false}, {3, 3, true, true},
                              {3, 3, true, false}});
    }
    else
        return false;
    out = a;
    return true;
}

int gainForLambda(double lambda)
{
    if (lambda < 0.0002)
        return 24;
    if (lambda < 0.0005)
        return 20;
    return 16;
}

size_t Model::latentCount() const
{
    size_t n = 0;
    for (const Grid& g : grids)
        n += (size_t)g.h * (size_t)g.w;
    return n;
}

int Model::tensorIndex(const std::string& name) const
{
    for (size_t i = 0; i < tensors.size(); i++)
        if (tensors[i].name == name)
            return (int)i;
    return -1;
}

bool buildModel(const Arch& arch, int height, int width, Model& out, std::string& error)
{
    if (height < 1 || width < 1 || height > MAX_SIDE || width > MAX_SIDE)
    {
        error = "picture size out of range";
        return false;
    }
    if (arch.latentHi < 0 || arch.latentHi > 12 || arch.armContext < 1 ||
        arch.armContext > MAX_CONTEXT || arch.armHidden < 0 || arch.armHidden > 8 || arch.ifceOut < 0 ||
        arch.synthesis.empty() || arch.upsK < 4 || arch.upsK > 16 || arch.upsK % 2 != 0 ||
        arch.preconcatK < 1 || arch.preconcatK > 15 || arch.preconcatK % 2 != 1)
    {
        error = "architecture out of range";
        return false;
    }

    Model m;
    m.arch = arch;
    m.height = height;
    m.width = width;

    // coolchic.py's post_init_latent(): each level's latent grid, then its
    // hyperlatent grid.
    int lo = 0;
    int hi = arch.latentHi;
    if (arch.hyper)
    {
        lo = std::min(lo, arch.hyperLo);
        hi = std::max(hi, arch.hyperHi);
    }
    for (int i = lo; i <= hi; i++)
    {
        Grid g;
        g.level = i;
        g.h = ceilShift(height, i);
        g.w = ceilShift(width, i);
        if (i <= arch.latentHi)
        {
            g.hyper = false;
            g.synIndex = (int)m.synGrid.size();
            m.synGrid.push_back((int)m.grids.size());
            m.grids.push_back(g);
        }
        if (arch.hyper && i >= arch.hyperLo && i <= arch.hyperHi)
        {
            g.hyper = true;
            g.synIndex = -1;
            m.grids.push_back(g);
        }
    }
    // post_init_ifce(): grids whose downsampling, ceil(log2(H / h)), falls
    // in the IFCE's range take the grids coarser than them as input.
    const int n = (int)m.grids.size();
    for (int i = 0; i < n; i++)
    {
        Grid& g = m.grids[(size_t)i];
        int ratio = (int)std::ceil(std::log2((double)height / (double)g.h));
        if (arch.ifceOut > 0 && ratio >= arch.ifceLo && ratio <= arch.ifceHi)
            g.ifceIn = std::max(n - 1 - i, 1);
        else
            g.ifceIn = 0;
    }

    // The context pixels in priority order (arm.py's
    // _get_non_zero_pixel_ctx_index), as offsets from the coded pixel.
    for (int p = 0; p < arch.armContext; p++)
        for (int idx = 0; idx < 40; idx++)
            if (PRIORITY[idx] == p)
            {
                m.ctxDy.push_back(idx / 9 - 4);
                m.ctxDx.push_back(idx % 9 - 4);
            }

    auto add = [&](const std::string& name, Module module, bool bias,
                   std::vector<int> shape, bool trained = true) {
        Tensor t;
        t.name = name;
        t.module = module;
        t.bias = bias;
        t.shape = shape;
        t.offset = m.count;
        t.size = 1;
        for (int s : shape)
            t.size *= (size_t)s;
        t.trained = trained;
        m.count += t.size;
        m.tensors.push_back(t);
        return (int)m.tensors.size() - 1;
    };

    for (int i = 0; i < n; i++)
        m.latent.push_back(add("latent_grids." + std::to_string(i), Module::Latent, false,
                               {1, 1, m.grids[(size_t)i].h, m.grids[(size_t)i].w}));

    // The ARM: hidden residual layers dim x dim, then dim -> 2, and the
    // stabiliser dim -> 2 beside them (arm.py's Arm).
    const int dim = arch.armInputs();
    int layer = 0;
    for (int i = 0; i < arch.armHidden; i++, layer += 2)
    {
        LinearRef l;
        l.in = dim;
        l.out = dim;
        l.residual = true;
        std::string p = "arm.mlp." + std::to_string(layer);
        l.weight = add(p + ".weight", Module::Arm, false, {dim, dim});
        l.bias = add(p + ".bias", Module::Arm, true, {dim});
        m.armLayers.push_back(l);
    }
    {
        LinearRef l;
        l.in = dim;
        l.out = 2;
        std::string p = "arm.mlp." + std::to_string(layer);
        l.weight = add(p + ".weight", Module::Arm, false, {2, dim});
        l.bias = add(p + ".bias", Module::Arm, true, {2});
        m.armLayers.push_back(l);
    }
    if (arch.armStabiliser)
    {
        m.armStabiliser.in = dim;
        m.armStabiliser.out = 2;
        m.armStabiliser.weight = add("arm.stabiliser_branch.weight", Module::Arm, false, {2, dim});
        m.armStabiliser.bias = add("arm.stabiliser_branch.bias", Module::Arm, true, {2});
    }

    // Upsampling: one transposed and one pre-concatenation kernel per step.
    // Their biases are parameters Cool-chic never uses or moves.
    const int halfUps = (arch.upsK + 1) / 2;
    const int halfPre = (arch.preconcatK + 1) / 2;
    for (int i = 0; i < arch.latentHi; i++)
    {
        std::string p = "upsampling.conv_transpose2ds." + std::to_string(i);
        m.upsBias.push_back(add(p + ".bias", Module::Upsampling, true, {1}, false));
        m.upsWeight.push_back(
            add(p + ".parametrizations.weight.original", Module::Upsampling, false, {halfUps}));
    }
    for (int i = 0; i < arch.latentHi; i++)
    {
        std::string p = "upsampling.conv2ds." + std::to_string(i);
        m.preBias.push_back(add(p + ".bias", Module::Upsampling, true, {1}, false));
        m.preWeight.push_back(
            add(p + ".parametrizations.weight.original", Module::Upsampling, false, {halfPre}));
    }

    // Synthesis: the main branch, then the stabiliser (1x1) beside it. The
    // output transform is the identity for RGB and never trained, so it is
    // left out here.
    if (arch.synOutputs() > 64)
    {
        error = "too many output channels";
        return false;
    }
    int in = arch.synInputs();
    for (size_t i = 0; i < arch.synthesis.size(); i++)
    {
        const SynLayer& s = arch.synthesis[i];
        if (s.out < 1 || s.out > 256 || (s.k != 1 && s.k != 3) || (s.residual && s.out != in))
        {
            error = "synthesis layer out of range";
            return false;
        }
        ConvRef c;
        c.layer = s;
        c.in = in;
        std::string p = "synthesis.main_branch." + std::to_string(2 * i);
        c.weight = add(p + ".weight", Module::Synthesis, false, {s.out, in, s.k, s.k});
        c.bias = add(p + ".bias", Module::Synthesis, true, {s.out});
        m.synLayers.push_back(c);
        in = s.out;
    }
    if (arch.synStabiliser)
    {
        SynLayer s;
        s.out = arch.synOutputs();
        s.k = 1;
        m.synStabiliser.layer = s;
        m.synStabiliser.in = arch.synInputs();
        m.synStabiliser.weight = add("synthesis.stabiliser_branch.weight", Module::Synthesis,
                                     false, {s.out, arch.synInputs(), 1, 1});
        m.synStabiliser.bias =
            add("synthesis.stabiliser_branch.bias", Module::Synthesis, true, {s.out});
    }

    // The output transform, sent with the synthesis but never trained:
    // the identity for RGB (synthesis.py's init_output_transform(None)).
    {
        const int c = arch.synOutputs();
        m.synOutput.layer.out = c;
        m.synOutput.layer.k = 1;
        m.synOutput.in = c;
        m.synOutput.weight = add("synthesis.output_transform.weight", Module::Synthesis, false,
                                 {c, c, 1, 1}, false);
        m.synOutput.bias =
            add("synthesis.output_transform.bias", Module::Synthesis, true, {c}, false);
    }

    // The IFCEs (no hidden layer, no stabiliser), one per grid that has one.
    m.ifce.assign((size_t)n, LinearRef());
    int ifceIndex = 0;
    for (int i = 0; i < n; i++)
    {
        const Grid& g = m.grids[(size_t)i];
        if (g.ifceIn <= 0)
            continue;
        LinearRef& l = m.ifce[(size_t)i];
        l.in = g.ifceIn;
        l.out = arch.ifceOut;
        std::string p = "ifce.arms." + std::to_string(ifceIndex++) + ".mlp.0";
        l.weight = add(p + ".weight", Module::Ifce, false, {arch.ifceOut, g.ifceIn});
        l.bias = add(p + ".bias", Module::Ifce, true, {arch.ifceOut});
    }

    out = std::move(m);
    return true;
}

void initParams(const Model& model, uint64_t seed, std::vector<float>& params)
{
    params.assign(model.count, 0.0f);
    Random rng(seed);

    auto fill = [&](int tensor, auto&& f) {
        if (tensor < 0)
            return;
        const Tensor& t = model.tensors[(size_t)tensor];
        for (size_t i = 0; i < t.size; i++)
            params[t.offset + i] = f();
    };

    // ArmLinear: biases zero; residual weights zero, others N(0, 1/out^4).
    auto initLinear = [&](const LinearRef& l) {
        if (l.weight < 0 || l.residual)
            return;
        const float sd = 1.0f / (float)(l.out * l.out);
        fill(l.weight, [&]() { return rng.normal() * sd; });
    };
    for (const LinearRef& l : model.armLayers)
        initLinear(l);
    initLinear(model.armStabiliser);
    for (const LinearRef& l : model.ifce)
        initLinear(l);

    // Upsampling: transposed kernels start bilinear (k < 8) or bicubic,
    // pre-concatenation kernels a Dirac.
    for (int t : model.upsWeight)
    {
        const Tensor& tt = model.tensors[(size_t)t];
        const float bilinear[2] = {0.25f, 0.75f};
        const float bicubic[4] = {0.0351562f, 0.1054687f, -0.2617187f, -0.8789063f};
        const float* core = model.arch.upsK < 8 ? bilinear : bicubic;
        const size_t coreN = model.arch.upsK < 8 ? 2 : 4;
        const size_t pad = tt.size > coreN ? tt.size - coreN : 0;
        for (size_t i = 0; i < tt.size; i++)
            params[tt.offset + i] = i >= pad ? core[i - pad] : 0.0f;
    }
    for (int t : model.preWeight)
    {
        const Tensor& tt = model.tensors[(size_t)t];
        params[tt.offset + tt.size - 1] = 1.0f;
    }

    // SynthesisConv2d: residual weights zero, others uniform in
    // +-sqrt(1 / (in k k)) / out^2.
    auto initConv = [&](const ConvRef& c) {
        if (c.weight < 0 || c.layer.residual)
            return;
        const double a = std::sqrt(1.0 / (double)(c.in * c.layer.k * c.layer.k)) /
                         (double)(c.layer.out * c.layer.out);
        fill(c.weight, [&]() { return (float)((rng.uniform() - 0.5) * 2.0 * a); });
    };
    for (const ConvRef& c : model.synLayers)
        initConv(c);
    initConv(model.synStabiliser);
    const int c = model.synOutput.layer.out;
    for (int i = 0; i < c; i++)
        params[model.tensors[(size_t)model.synOutput.weight].offset + (size_t)(i * c + i)] = 1.0f;
}

// xoshiro256** (Blackman and Vigna), seeded through splitmix64.
Random::Random(uint64_t seed)
{
    uint64_t z = seed;
    for (int i = 0; i < 4; i++)
    {
        z += 0x9e3779b97f4a7c15ULL;
        uint64_t x = z;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        s_[i] = x ^ (x >> 31);
    }
}

uint64_t Random::next()
{
    auto rotl = [](uint64_t x, int k) { return (x << k) | (x >> (64 - k)); };
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Random::uniform()
{
    return (double)(next() >> 11) * (1.0 / 9007199254740992.0);
}

// Marsaglia's polar method in single precision, two normals from each
// 64-bit draw that lands inside the unit circle.
float Random::normal()
{
    if (haveSpare_)
    {
        haveSpare_ = false;
        return spare_;
    }
    for (;;)
    {
        const uint64_t r = next();
        const float u = (float)(r >> 40) * (2.0f / 16777216.0f) - 1.0f;
        const float v = (float)((r >> 16) & 0xFFFFFF) * (2.0f / 16777216.0f) - 1.0f;
        const float s = u * u + v * v;
        if (s >= 1.0f || s == 0.0f)
            continue;
        const float f = std::sqrt(-2.0f * std::log(s) / s);
        spare_ = v * f;
        haveSpare_ = true;
        return u * f;
    }
}

void Random::normals(float* out, size_t count)
{
    for (size_t i = 0; i < count; i++)
        out[i] = normal();
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
