// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/component/core/, coolchic/training/loss.py), backward passes
// written by hand. Authors: see Cool-chic's CONTRIBUTORS.md.
#include "FitNet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

constexpr int MAX_DIM = 64;         // ARM inputs at most (vhop: 26)
constexpr float LOG_SCALE_MIN = -5.0f;
constexpr float LOG_SCALE_MAX = 5.0f;
constexpr float LOG_SHIFT = -4.0f;
constexpr double MIN_PROBA = 1.0 / 65536.0;    // no latent costs more than 16 bits
constexpr double LN2 = 0.69314718055994530942;

// quantizer.py's softround(): floor(x) + tanh((x - floor(x) - 1/2) / t)
// / (2 tanh(1 / 2t)) + 1/2, and its derivative.
// tanh as 1 - 2 / (1 + exp(2z)), twice as fast as tanhf() and within 2e-7
// of it.
inline float softRound(float x, float invT, float invTanh, float& slope)
{
    const float f = std::floor(x);
    const float e = std::exp(2.0f * (x - f - 0.5f) * invT);
    const float th = 1.0f - 2.0f / (1.0f + e);
    slope = 0.5f * (1.0f - th * th) * invT * invTanh;
    return f + 0.5f * th * invTanh + 0.5f;
}

// The full kernel of a symmetric half-kernel: a b c d -> a b c d d c b a
// (even k) or a b c d c b a (odd k).
void expandKernel(const float* half, int k, float* full)
{
    for (int t = 0; t < k; t++)
        full[t] = half[std::min(t, k - 1 - t)];
}

void foldKernelGrad(const double* full, int k, float* half)
{
    for (int t = 0; t < k; t++)
        half[std::min(t, k - 1 - t)] += (float)full[t];
}

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Dot products and sums in single precision with sixteen running sums, so
// the compiler can vectorise them (a single running sum must be added in
// order, one element at a time).
inline float dot(const float* a, const float* b, int n)
{
    float acc[16] = {};
    int x = 0;
    for (; x + 16 <= n; x += 16)
        for (int j = 0; j < 16; j++)
            acc[j] += a[x + j] * b[x + j];
    float s = 0.0f;
    for (; x < n; x++)
        s += a[x] * b[x];
    for (int j = 0; j < 16; j++)
        s += acc[j];
    return s;
}

inline float sum(const float* a, int n)
{
    float acc[16] = {};
    int x = 0;
    for (; x + 16 <= n; x += 16)
        for (int j = 0; j < 16; j++)
            acc[j] += a[x + j];
    float s = 0.0f;
    for (; x < n; x++)
        s += a[x];
    for (int j = 0; j < 16; j++)
        s += acc[j];
    return s;
}

// The transposed convolution (upsampling.py, training path: pad P0 = k/2
// replicated, conv_transpose2d stride 2, crop C = k - 1 + k/2): output o
// reads input clamp((o + C - t) / 2 - P0) through tap t, for the t with
// o + C - t even. Output 2m + parity reads input m + off[j] through tap
// t[j], j < k / 2.
struct UpTaps
{
    int k = 0;
    int c = 0;
    int p0 = 0;
    int n[2] = {0, 0};
    int t[2][8] = {};
    int off[2][8] = {};

    explicit UpTaps(int kernel) : k(kernel), c(kernel - 1 + kernel / 2), p0(kernel / 2)
    {
        for (int par = 0; par < 2; par++)
            for (int tap = 0; tap < k; tap++)
                if (((par + c - tap) & 1) == 0)
                {
                    t[par][n[par]] = tap;
                    off[par][n[par]] = (par + c - tap) / 2 - p0;
                    n[par]++;
                }
    }

    // The input row read for output row o through tap t (unclamped).
    int raw(int o, int tap) const { return (o + c - tap) / 2 - p0; }
};

// Replicated padding on each side of a row: enough for any tap's offset.
constexpr int PAD = 16;

void padReplicate(const float* src, int w, float* out)
{
    for (int j = 0; j < PAD; j++)
    {
        out[j] = src[0];
        out[PAD + w + j] = src[w - 1];
    }
    std::copy(src, src + w, out + PAD);
}

// Horizontal transposed convolution of one row: w in, tw out. scratch
// holds at least 2 * (w + 2 PAD) floats.
void upRow(const float* src, int w, const float* k, const UpTaps& u, float* out, int tw,
           float* scratch)
{
    float* sp = scratch;
    float* half = scratch + w + 2 * PAD;
    padReplicate(src, w, sp);
    for (int par = 0; par < 2; par++)
    {
        const int nm = (tw - par + 1) / 2;
        std::fill(half, half + nm, 0.0f);
        for (int j = 0; j < u.n[par]; j++)
        {
            const float kt = k[u.t[par][j]];
            const float* s = sp + PAD + u.off[par][j];
            for (int m = 0; m < nm; m++)
                half[m] += kt * s[m];
        }
        for (int m = 0; m < nm; m++)
            out[2 * m + par] = half[m];
    }
}

// Its transpose: the gradient of the w inputs (written, not added) from
// that of the tw outputs, and the kernel's gradient (added to tk).
void upRowBack(const float* src, int w, const float* k, const UpTaps& u, const float* gt, int tw,
               float* gin, double* tk, float* scratch)
{
    float* sp = scratch;
    float* gpad = sp + w + 2 * PAD;
    float* half = gpad + w + 2 * PAD;
    padReplicate(src, w, sp);
    std::fill(gpad, gpad + w + 2 * PAD, 0.0f);
    for (int par = 0; par < 2; par++)
    {
        const int nm = (tw - par + 1) / 2;
        for (int m = 0; m < nm; m++)
            half[m] = gt[2 * m + par];
        for (int j = 0; j < u.n[par]; j++)
        {
            const int t = u.t[par][j];
            const int off = PAD + u.off[par][j];
            float* g = gpad + off;
            const float kt = k[t];
            for (int m = 0; m < nm; m++)
                g[m] += kt * half[m];
            tk[t] += dot(half, sp + off, nm);
        }
    }
    std::copy(gpad + PAD, gpad + PAD + w, gin);
    gin[0] += sum(gpad, PAD);
    gin[w - 1] += sum(gpad + PAD + w, PAD);
}

// The input rows (clamped) that output rows read through tap t, for input
// row i: the outputs o = 2 (r + P0) - C + t over the raw rows r from r0 to
// r1 that clamp to i.
void upReaders(const UpTaps& u, int tap, int i, int h, int th, int& o0, int& o1)
{
    const int first = (u.c + tap) & 1;     // the first output with the right parity
    const int last = th - 1 - ((th - 1 - first) & 1);
    if (last < first)
    {
        o0 = 0;
        o1 = -2;
        return;
    }
    const int rawMin = u.raw(first, tap);
    const int rawMax = u.raw(last, tap);
    const int r0 = std::max(i == 0 ? rawMin : i, rawMin);
    const int r1 = std::min(i == h - 1 ? rawMax : i, rawMax);
    o0 = 2 * (r0 + u.p0) - u.c + tap;
    o1 = 2 * (r1 + u.p0) - u.c + tap;
}

// The pre-concatenation convolution's horizontal pass of one row (odd k,
// zero padding). scratch holds w + 2 PAD floats.
void preRow(const float* src, int w, const float* k, int kk, float* out, float* scratch)
{
    const int p = kk / 2;
    std::fill(scratch, scratch + PAD, 0.0f);
    std::fill(scratch + PAD + w, scratch + 2 * PAD + w, 0.0f);
    std::copy(src, src + w, scratch + PAD);
    std::fill(out, out + w, 0.0f);
    for (int j = 0; j < kk; j++)
    {
        const float* s = scratch + PAD + j - p;
        const float kj = k[j];
        for (int x = 0; x < w; x++)
            out[x] += kj * s[x];
    }
}

// Its transpose: the gradient of the input row (written) and the kernel's.
void preRowBack(const float* src, int w, const float* k, int kk, const float* gt, float* gin,
                double* tk, float* scratch)
{
    const int p = kk / 2;
    float* sp = scratch;
    float* gpad = scratch + w + 2 * PAD;
    std::fill(sp, sp + PAD, 0.0f);
    std::fill(sp + PAD + w, sp + 2 * PAD + w, 0.0f);
    std::copy(src, src + w, sp + PAD);
    std::fill(gpad, gpad + w + 2 * PAD, 0.0f);
    for (int j = 0; j < kk; j++)
    {
        float* g = gpad + PAD + j - p;
        const float kj = k[j];
        for (int x = 0; x < w; x++)
            g[x] += kj * gt[x];
        tk[j] += dot(gt, sp + PAD + j - p, w);
    }
    std::copy(gpad + PAD, gpad + PAD + w, gin);
}

} // namespace

Net::Net(const Model& model, Pool& pool)
    : m_(model),
      pool_(pool),
      threads_(pool.threads()),
      netOffset_(model.tensors[(size_t)model.latent.back()].offset +
                 model.tensors[(size_t)model.latent.back()].size),
      dim_(model.arch.armInputs())
{
    size_t off = 0;
    for (size_t i = 0; i < m_.grids.size(); i++)
    {
        gridOff_.push_back(off);
        off += (size_t)m_.grids[i].h * (size_t)m_.grids[i].w;
        for (int y = 0; y < m_.grids[i].h; y++)
        {
            rowGrid_.push_back((int)i);
            rowY_.push_back(y);
        }
    }
    q_.assign(off, 0.0f);
    dq_.assign(off, 0.0f);
    gq_.assign(off, 0.0f);
    dctx_.assign(off * (size_t)dim_, 0.0f);
    armScratch_.assign((size_t)threads_,
                       std::vector<float>((size_t)(m_.arch.armHidden + 4) * (size_t)dim_ *
                                              ARM_BLOCK +
                                          4 * ARM_BLOCK,
                                          0.0f));
    ifceF_.resize(m_.grids.size());
    for (size_t i = 0; i < m_.grids.size(); i++)
        if (m_.grids[i].ifceIn > 0 && i + 1 < m_.grids.size())
            ifceF_[i].assign((size_t)m_.arch.ifceOut * (size_t)m_.grids[i + 1].h *
                                 (size_t)m_.grids[i + 1].w,
                             0.0f);
    threadGrad_.assign((size_t)threads_, std::vector<float>(m_.count - netOffset_, 0.0f));
    threadSum_.assign((size_t)threads_, 0.0);
    const int L = m_.arch.latentHi;
    const size_t P = (size_t)m_.height * (size_t)m_.width;
    ups_.resize((size_t)L);
    for (int step = 0; step < L; step++)
    {
        UpStep& st = ups_[(size_t)step];
        const FitGrid& from = m_.grids[(size_t)m_.synGrid[(size_t)(L - step)]];
        const FitGrid& to = m_.grids[(size_t)m_.synGrid[(size_t)(L - 1 - step)]];
        st.h = from.h;
        st.w = from.w;
        st.th = to.h;
        st.tw = to.w;
        const size_t in = (size_t)st.h * (size_t)st.w;
        const size_t tmp = (size_t)st.h * (size_t)st.tw;
        const size_t out = (size_t)st.th * (size_t)st.tw;
        st.in.assign((size_t)step + 1, std::vector<float>(in, 0.0f));
        st.gin = st.in;
        st.tmp.assign((size_t)step + 1, std::vector<float>(tmp, 0.0f));
        st.gtmp = st.tmp;
        st.preIn.assign(out, 0.0f);
        st.preTmp = st.preIn;
        st.preGtmp = st.preIn;
    }
    z_.assign((size_t)L + 1, std::vector<float>(P, 0.0f));
    gz_ = z_;
    rowScratch_.assign((size_t)threads_,
                       std::vector<float>(
                           std::max<size_t>(4096, 4 * ((size_t)m_.width + 2 * (size_t)PAD)),
                           0.0f));
    kernelGrad_.assign((size_t)threads_ * 32, 0.0);
}

void Net::quantise(const float* params, const StepConfig& config, bool wantGrad)
{
    const size_t n = q_.size();
    const float* y = params;    // the latents lead the parameters, grid by grid
    const float gain = (float)m_.gain;
    if (config.hardRound)
    {
        pool_.run((int)((n + 4095) / 4096), [&](int b0, int b1, int) {
            for (size_t i = (size_t)b0 * 4096; i < std::min(n, (size_t)b1 * 4096); i++)
            {
                q_[i] = std::nearbyint(gain * y[i]);     // ties to even, as torch.round
                dq_[i] = 0.0f;
            }
        });
        return;
    }
    // softround(softround(x, t) + noise, t), Gaussian noise (quantize()).
    const float invT = 1.0f / config.temperature;
    const float invTanh = 1.0f / std::tanh(0.5f * invT);
    const int blocks = (int)((n + 4095) / 4096);
    pool_.run(blocks, [&](int b0, int b1, int thread) {
        float* noise = rowScratch_[(size_t)thread].data();     // at least 4096
        for (int b = b0; b < b1; b++)
        {
            const size_t i0 = (size_t)b * 4096;
            const size_t i1 = std::min(n, i0 + 4096);
            if (config.noise > 0.0f)
            {
                Random rng(config.seed * 0x9E3779B97F4A7C15ULL + (uint64_t)b);
                rng.normals(noise, i1 - i0);
            }
            else
                std::fill(noise, noise + (i1 - i0), 0.0f);
            for (size_t i = i0; i < i1; i++)
            {
                float s1, s2;
                const float x = gain * y[i];
                const float a = softRound(x, invT, invTanh, s1);
                q_[i] = softRound(a + config.noise * noise[i - i0], invT, invTanh, s2);
                dq_[i] = wantGrad ? gain * s1 * s2 : 0.0f;
            }
        }
    });
}

void Net::quantiseHard(const float* params)
{
    StepConfig c;
    c.hardRound = true;
    quantise(params, c, false);
}

// The IFCE's input for grid g: the grids coarser than it, nearest-upsampled
// to grid g + 1's size (coolchic.py's fixed_upsampling(mode="nearest")),
// grid g + 1 + c in channel c; its output, at that size, is read at
// (y / 2, x / 2) by grid g's pixels.
void Net::ifceForward(const float* params)
{
    const int n = (int)m_.grids.size();
    for (int g = 0; g < n; g++)
    {
        const LinearRef& l = m_.ifce[(size_t)g];
        if (l.weight < 0 || g + 1 >= n)
            continue;
        const FitGrid& at = m_.grids[(size_t)g + 1];
        const float* W = params + m_.tensors[(size_t)l.weight].offset;
        const float* B = params + m_.tensors[(size_t)l.bias].offset;
        float* F = ifceF_[(size_t)g].data();
        const size_t plane = (size_t)at.h * (size_t)at.w;
        pool_.run(at.h, [&](int r0, int r1, int) {
            float v[MAX_DIM + 64];
            for (int y = r0; y < r1; y++)
                for (int x = 0; x < at.w; x++)
                {
                    for (int c = 0; c < l.in; c++)
                    {
                        const int j = g + 1 + c;
                        if (j >= n)
                        {
                            v[c] = 0.0f;
                            continue;
                        }
                        const FitGrid& src = m_.grids[(size_t)j];
                        const int d = src.level - at.level;
                        v[c] = q_[gridOff_[(size_t)j] + (size_t)(y >> d) * (size_t)src.w +
                                  (size_t)(x >> d)];
                    }
                    for (int o = 0; o < l.out; o++)
                    {
                        float acc = B[o];
                        for (int c = 0; c < l.in; c++)
                            acc += W[o * l.in + c] * v[c];
                        F[(size_t)o * plane + (size_t)y * (size_t)at.w + (size_t)x] = acc;
                    }
                }
        });
    }
}

// The ARM over every latent: contexts, MLP, Laplace rate, and with
// wantGrad the rate's gradient with respect to the contexts (into dctx_,
// planar: context c of latent i at c * latents + i), the coded value (into
// gq_) and the ARM's weights (per thread). Latents go through in blocks of
// up to ARM_BLOCK along a row, values laid out [channel][latent], so the
// layers run over the block in the innermost loop.
template <int DT>
void Net::armRows(const ArmLayers& a, int r0, int r1, int thread, bool wantGrad)
{
    // Bounded, so the compiler knows the blocks below are small.
    const int D = DT > 0 ? DT : std::min(std::max(dim_, 1), MAX_DIM);
    const int S = m_.arch.armContext;
    const int nh = m_.arch.armHidden;
    const size_t N = q_.size();
    constexpr int NB = ARM_BLOCK;
    float* tg = wantGrad ? threadGrad_[(size_t)thread].data() : nullptr;
    // Scratch: h[0..nh] (h[0] the input), gh, ga, gx, each [D][NB]; then
    // the outputs and their gradients, and the block's weight gradients.
    float* scratch = armScratch_[(size_t)thread].data();
    const size_t act = (size_t)D * NB;
    float* h = scratch;
    float* gh = h + (size_t)(nh + 1) * act;
    float* ga = gh + act;
    float* gx = ga + act;
    float* o0 = gx + act;
    float* o1 = o0 + NB;
    float* g0 = o1 + NB;
    float* g1 = g0 + NB;
    double bits = 0.0;

    for (int r = r0; r < r1; r++)
    {
        const int g = rowGrid_[(size_t)r];
        const int y = rowY_[(size_t)r];
        const FitGrid& grid = m_.grids[(size_t)g];
        const float* qg = q_.data() + gridOff_[(size_t)g];
        const bool hasIfce = !ifceF_[(size_t)g].empty();
        const int fw = hasIfce ? m_.grids[(size_t)g + 1].w : 0;
        const size_t fplane = hasIfce ? (size_t)m_.grids[(size_t)g + 1].h * (size_t)fw : 0;
        const size_t rowBase = gridOff_[(size_t)g] + (size_t)y * (size_t)grid.w;
        for (int x0 = 0; x0 < grid.w; x0 += NB)
        {
            const int nb = std::min(NB, grid.w - x0);
            // The contexts.
            for (int c = 0; c < S; c++)
            {
                float* in = h + (size_t)c * NB;
                const int yy = y + m_.ctxDy[(size_t)c];
                const int dx = m_.ctxDx[(size_t)c];
                if (yy < 0 || yy >= grid.h)
                {
                    std::fill(in, in + nb, 0.0f);
                    continue;
                }
                const float* src = qg + (size_t)yy * (size_t)grid.w;
                for (int b = 0; b < nb; b++)
                {
                    const int xx = x0 + b + dx;
                    in[b] = (xx >= 0 && xx < grid.w) ? src[xx] : 0.0f;
                }
            }
            for (int c = S; c < D; c++)
            {
                float* in = h + (size_t)c * NB;
                if (!hasIfce)
                {
                    std::fill(in, in + nb, 0.0f);
                    continue;
                }
                const float* src = ifceF_[(size_t)g].data() + (size_t)(c - S) * fplane +
                                   (size_t)(y >> 1) * (size_t)fw;
                for (int b = 0; b < nb; b++)
                    in[b] = src[(x0 + b) >> 1];
            }
            // Hidden residual layers with ReLU.
            for (int l = 0; l < nh; l++)
            {
                const float* W = a.layer[l].W;
                const float* B = a.layer[l].B;
                float* hi = h + (size_t)l * act;
                float* ho = hi + act;
                for (int o = 0; o < D; o++)
                {
                    float* dst = ho + (size_t)o * NB;
                    const float* res = hi + (size_t)o * NB;
                    for (int b = 0; b < nb; b++)
                        dst[b] = B[o] + res[b];
                    for (int c = 0; c < D; c++)
                    {
                        const float w = W[o * D + c];
                        const float* src = hi + (size_t)c * NB;
                        for (int b = 0; b < nb; b++)
                            dst[b] += w * src[b];
                    }
                    for (int b = 0; b < nb; b++)
                        dst[b] = dst[b] > 0.0f ? dst[b] : 0.0f;
                }
            }
            const float* hl = h + (size_t)nh * act;
            const ArmLinear& out = a.layer[nh];
            for (int b = 0; b < nb; b++)
            {
                o0[b] = out.B[0];
                o1[b] = out.B[1];
            }
            for (int c = 0; c < D; c++)
            {
                const float w0 = out.W[c], w1 = out.W[D + c];
                const float* src = hl + (size_t)c * NB;
                for (int b = 0; b < nb; b++)
                {
                    o0[b] += w0 * src[b];
                    o1[b] += w1 * src[b];
                }
            }
            if (a.stab.W)
            {
                for (int b = 0; b < nb; b++)
                {
                    o0[b] += a.stab.B[0];
                    o1[b] += a.stab.B[1];
                }
                for (int c = 0; c < D; c++)
                {
                    const float w0 = a.stab.W[c], w1 = a.stab.W[D + c];
                    const float* src = h + (size_t)c * NB;
                    for (int b = 0; b < nb; b++)
                    {
                        o0[b] += w0 * src[b];
                        o1[b] += w1 * src[b];
                    }
                }
            }
            // Laplace(mu, b) over [q - 1/2, q + 1/2], in single precision
            // the way arm.py's _laplace_cdf() has it: 1/2 - sign(d)
            // expm1(-|d| / b) / 2. Above mu its tail is 1 minus a small
            // number, so far out the probability rounds to nothing and the
            // rate stops at 16 bits; Cool-chic fits with that, so this does
            // too.
            bool anyGrad = false;
            for (int b = 0; b < nb; b++)
            {
                const float mu = o0[b];
                const float ls = o1[b] + LOG_SHIFT;
                const float lsc = std::min(std::max(ls, LOG_SCALE_MIN), LOG_SCALE_MAX);
                const float bf = std::exp(lsc);
                const double sc = bf;
                const size_t idx = rowBase + (size_t)(x0 + b);
                const float qv = q_[idx];
                const float d1 = qv + 0.5f - mu;
                const float d0 = qv - 0.5f - mu;
                const float m1 = std::expm1(-std::fabs(d1) / bf);
                const float m0 = std::expm1(-std::fabs(d0) / bf);
                auto cdf = [](float d, float em1) {
                    const float sign = d > 0.0f ? 1.0f : (d < 0.0f ? -1.0f : 0.0f);
                    return 0.5f - 0.5f * sign * em1;
                };
                const float pf = cdf(d1, m1) - cdf(d0, m0);
                const bool clamped = pf < (float)MIN_PROBA;
                const double p = clamped ? MIN_PROBA : (double)pf;
                bits += -std::log(p) / LN2;
                if (!wantGrad)
                    continue;
                if (clamped)
                {
                    g0[b] = g1[b] = 0.0f;
                    gq_[idx] = 0.0f;
                    continue;
                }
                const double gp = -a.rateWeight / (p * LN2);
                const double pdf1 = 0.5 / sc * ((double)m1 + 1.0);
                const double pdf0 = 0.5 / sc * ((double)m0 + 1.0);
                const double dpdq = pdf1 - pdf0;
                const double dpdb = -(d1 / sc) * pdf1 + (d0 / sc) * pdf0;
                gq_[idx] = (float)(gp * dpdq);
                g0[b] = (float)(-gp * dpdq);    // dLoss/dmu
                const bool inRange = ls >= LOG_SCALE_MIN && ls <= LOG_SCALE_MAX;
                g1[b] = inRange ? (float)(gp * dpdb * sc) : 0.0f;
                anyGrad = true;
            }
            if (!wantGrad)
                continue;
            float* dctx = dctx_.data() + rowBase + (size_t)x0;
            if (!anyGrad)
            {
                for (int c = 0; c < D; c++)
                    std::fill(dctx + (size_t)c * N, dctx + (size_t)c * N + nb, 0.0f);
                continue;
            }

            // Back through the output layer and the stabiliser.
            const float s0 = sum(g0, nb), s1 = sum(g1, nb);
            tg[out.gb] += s0;
            tg[out.gb + 1] += s1;
            for (int c = 0; c < D; c++)
            {
                const float* src = hl + (size_t)c * NB;
                float* dst = gh + (size_t)c * NB;
                const float w0 = out.W[c], w1 = out.W[D + c];
                for (int b = 0; b < nb; b++)
                    dst[b] = w0 * g0[b] + w1 * g1[b];
                tg[out.gw + (size_t)c] += dot(g0, src, nb);
                tg[out.gw + (size_t)(D + c)] += dot(g1, src, nb);
            }
            if (a.stab.W)
            {
                tg[a.stab.gb] += s0;
                tg[a.stab.gb + 1] += s1;
                for (int c = 0; c < D; c++)
                {
                    const float* src = h + (size_t)c * NB;
                    float* dst = gx + (size_t)c * NB;
                    const float w0 = a.stab.W[c], w1 = a.stab.W[D + c];
                    for (int b = 0; b < nb; b++)
                        dst[b] = w0 * g0[b] + w1 * g1[b];
                    tg[a.stab.gw + (size_t)c] += dot(g0, src, nb);
                    tg[a.stab.gw + (size_t)(D + c)] += dot(g1, src, nb);
                }
            }
            else
                std::fill(gx, gx + act, 0.0f);
            // Back through the hidden layers: h[l+1] = relu(W h[l] + b + h[l]).
            for (int l = nh - 1; l >= 0; l--)
            {
                const ArmLinear& ly = a.layer[l];
                const float* hi = h + (size_t)l * act;
                const float* ho = hi + act;
                for (int o = 0; o < D; o++)
                {
                    const float* src = ho + (size_t)o * NB;
                    const float* gsrc = gh + (size_t)o * NB;
                    float* dst = ga + (size_t)o * NB;
                    for (int b = 0; b < nb; b++)
                        dst[b] = src[b] > 0.0f ? gsrc[b] : 0.0f;
                    tg[ly.gb + (size_t)o] += sum(dst, nb);
                    float* gw = tg + ly.gw + (size_t)o * (size_t)D;
                    for (int c = 0; c < D; c++)
                        gw[c] += dot(dst, hi + (size_t)c * NB, nb);
                }
                for (int c = 0; c < D; c++)
                {
                    float* dst = gh + (size_t)c * NB;
                    const float* res = ga + (size_t)c * NB;
                    for (int b = 0; b < nb; b++)
                        dst[b] = res[b];
                    for (int o = 0; o < D; o++)
                    {
                        const float w = ly.W[o * D + c];
                        const float* src = ga + (size_t)o * NB;
                        for (int b = 0; b < nb; b++)
                            dst[b] += w * src[b];
                    }
                }
            }
            for (int c = 0; c < D; c++)
            {
                const float* p0 = gh + (size_t)c * NB;
                const float* p1 = gx + (size_t)c * NB;
                float* dst = dctx + (size_t)c * N;
                for (int b = 0; b < nb; b++)
                    dst[b] = p0[b] + p1[b];
            }
        }
    }
    threadSum_[(size_t)thread] += bits;
}

double Net::armPass(const float* params, double rateWeight, bool wantGrad)
{
    ArmLayers a;
    a.rateWeight = rateWeight;
    int i = 0;
    for (const LinearRef& r : m_.armLayers)
        a.layer[i++] = {params + m_.tensors[(size_t)r.weight].offset,
                        params + m_.tensors[(size_t)r.bias].offset,
                        m_.tensors[(size_t)r.weight].offset - netOffset_,
                        m_.tensors[(size_t)r.bias].offset - netOffset_};
    if (m_.armStabiliser.weight >= 0)
        a.stab = {params + m_.tensors[(size_t)m_.armStabiliser.weight].offset,
                  params + m_.tensors[(size_t)m_.armStabiliser.bias].offset,
                  m_.tensors[(size_t)m_.armStabiliser.weight].offset - netOffset_,
                  m_.tensors[(size_t)m_.armStabiliser.bias].offset - netOffset_};
    std::fill(threadSum_.begin(), threadSum_.end(), 0.0);
    pool_.run((int)rowGrid_.size(), [&](int r0, int r1, int thread) {
        switch (dim_)
        {
        case 8: armRows<8>(a, r0, r1, thread, wantGrad); break;
        case 14: armRows<14>(a, r0, r1, thread, wantGrad); break;
        case 20: armRows<20>(a, r0, r1, thread, wantGrad); break;
        case 26: armRows<26>(a, r0, r1, thread, wantGrad); break;
        default: armRows<0>(a, r0, r1, thread, wantGrad); break;
        }
    });
    double total = 0.0;
    for (double s : threadSum_)
        total += s;
    return total;
}

// Gathers the context gradients back onto the latents they were read from.
void Net::contextBackward()
{
    const size_t N = q_.size();
    const int S = m_.arch.armContext;
    const int nRows = (int)rowGrid_.size();
    pool_.run(nRows, [&](int r0, int r1, int) {
        for (int r = r0; r < r1; r++)
        {
            const int g = rowGrid_[(size_t)r];
            const int y = rowY_[(size_t)r];
            const FitGrid& grid = m_.grids[(size_t)g];
            const size_t base = gridOff_[(size_t)g];
            for (int x = 0; x < grid.w; x++)
            {
                float acc = 0.0f;
                for (int c = 0; c < S; c++)
                {
                    // The pixel that read (y, x) as its context c.
                    const int yy = y - m_.ctxDy[(size_t)c];
                    const int xx = x - m_.ctxDx[(size_t)c];
                    if (yy >= 0 && yy < grid.h && xx >= 0 && xx < grid.w)
                        acc += dctx_[(size_t)c * N + base + (size_t)yy * (size_t)grid.w +
                                     (size_t)xx];
                }
                gq_[base + (size_t)y * (size_t)grid.w + (size_t)x] += acc;
            }
        }
    });
}

void Net::ifceBackward(const float* params, float* grads)
{
    const size_t N = q_.size();
    const int S = m_.arch.armContext;
    const int n = (int)m_.grids.size();
    for (int g = 0; g < n; g++)
    {
        const LinearRef& l = m_.ifce[(size_t)g];
        if (l.weight < 0 || g + 1 >= n)
            continue;
        const FitGrid& me = m_.grids[(size_t)g];
        const FitGrid& at = m_.grids[(size_t)g + 1];
        const float* W = params + m_.tensors[(size_t)l.weight].offset;
        float* gW = grads + m_.tensors[(size_t)l.weight].offset;
        float* gB = grads + m_.tensors[(size_t)l.bias].offset;
        std::vector<float> gF((size_t)l.out * (size_t)at.h * (size_t)at.w, 0.0f);
        const size_t plane = (size_t)at.h * (size_t)at.w;
        // dLoss/dF: each F pixel feeds the 2 x 2 pixels of grid g above it.
        for (int y = 0; y < me.h; y++)
            for (int x = 0; x < me.w; x++)
            {
                const float* dctx =
                    dctx_.data() + gridOff_[(size_t)g] + (size_t)y * (size_t)me.w + (size_t)x;
                const size_t f = (size_t)(y >> 1) * (size_t)at.w + (size_t)(x >> 1);
                for (int o = 0; o < l.out; o++)
                    gF[(size_t)o * plane + f] += dctx[(size_t)(S + o) * N];
            }
        // Back through the linear layer onto the coarser grids.
        std::vector<double> gw((size_t)l.out * (size_t)l.in, 0.0), gb((size_t)l.out, 0.0);
        for (int y = 0; y < at.h; y++)
            for (int x = 0; x < at.w; x++)
            {
                float go[MAX_DIM];
                bool any = false;
                for (int o = 0; o < l.out; o++)
                {
                    go[o] = gF[(size_t)o * plane + (size_t)y * (size_t)at.w + (size_t)x];
                    any = any || go[o] != 0.0f;
                }
                if (!any)
                    continue;
                for (int c = 0; c < l.in; c++)
                {
                    const int j = g + 1 + c;
                    if (j >= n)
                        continue;
                    const FitGrid& src = m_.grids[(size_t)j];
                    const int d = src.level - at.level;
                    const size_t idx =
                        gridOff_[(size_t)j] + (size_t)(y >> d) * (size_t)src.w + (size_t)(x >> d);
                    const float v = q_[idx];
                    float acc = 0.0f;
                    for (int o = 0; o < l.out; o++)
                    {
                        gw[(size_t)o * (size_t)l.in + (size_t)c] += (double)go[o] * v;
                        acc += W[o * l.in + c] * go[o];
                    }
                    gq_[idx] += acc;
                }
                for (int o = 0; o < l.out; o++)
                    gb[(size_t)o] += go[o];
            }
        for (size_t i = 0; i < gw.size(); i++)
            gW[i] += (float)gw[i];
        for (size_t i = 0; i < gb.size(); i++)
            gB[i] += (float)gb[i];
    }
}

// upsampling.py's Upsampling.forward(): from the coarsest synthesis grid,
// each step upsamples every channel so far by 2 and puts the next finer
// grid, through its pre-concatenation filter, in front. The state before
// step i is ups_[i].in; after the last, z_.
void Net::upsampleForward(const float* params, int dropFinest)
{
    const int L = m_.arch.latentHi;
    const int K = m_.arch.upsK;
    const int KP = m_.arch.preconcatK;
    const UpTaps u(K);

    // A synthesis grid's latents, or zeros for one progressive fitting drops.
    auto gridPlane = [&](int s, std::vector<float>& v) {
        const int g = m_.synGrid[(size_t)s];
        if (s >= dropFinest)
            std::copy(q_.begin() + (long)gridOff_[(size_t)g],
                      q_.begin() + (long)(gridOff_[(size_t)g] + v.size()), v.begin());
        else
            std::fill(v.begin(), v.end(), 0.0f);
    };
    if (L == 0)
    {
        gridPlane(0, z_[0]);
        return;
    }
    gridPlane(L, ups_[0].in[0]);
    for (int step = 0; step < L; step++)
    {
        const int s = L - 1 - step;
        UpStep& st = ups_[(size_t)step];
        std::vector<std::vector<float>>& next =
            step + 1 < L ? ups_[(size_t)step + 1].in : z_;
        const int h = st.h, w = st.w, th = st.th, tw = st.tw;
        const int nc = (int)st.in.size();
        float k[16], kp[16];
        expandKernel(params + m_.tensors[(size_t)m_.upsWeight[(size_t)step]].offset, K, k);
        expandKernel(params + m_.tensors[(size_t)m_.preWeight[(size_t)step]].offset, KP, kp);
        gridPlane(s, st.preIn);

        // Horizontal passes: the target's rows, then each channel's.
        pool_.run(th + nc * h, [&](int r0, int r1, int thread) {
            float* scratch = rowScratch_[(size_t)thread].data();
            for (int r = r0; r < r1; r++)
            {
                if (r < th)
                {
                    preRow(st.preIn.data() + (size_t)r * (size_t)tw, tw, kp, KP,
                           st.preTmp.data() + (size_t)r * (size_t)tw, scratch);
                    continue;
                }
                const int c = (r - th) / h, y = (r - th) % h;
                upRow(st.in[(size_t)c].data() + (size_t)y * (size_t)w, w, k, u,
                      st.tmp[(size_t)c].data() + (size_t)y * (size_t)tw, tw, scratch);
            }
        });
        // Vertical passes: the target's (zero padded, with its residual) into
        // channel 0, the upsampled channels behind it.
        pool_.run(th * (nc + 1), [&](int r0, int r1, int) {
            for (int r = r0; r < r1; r++)
            {
                const int c = r / th, o = r % th;
                float* dst = next[(size_t)c].data() + (size_t)o * (size_t)tw;
                if (c == 0)
                {
                    const float* res = st.preIn.data() + (size_t)o * (size_t)tw;
                    std::copy(res, res + tw, dst);
                    const int p = KP / 2;
                    for (int j = 0; j < KP; j++)
                    {
                        const int i = o + j - p;
                        if (i < 0 || i >= th)
                            continue;
                        const float* src = st.preTmp.data() + (size_t)i * (size_t)tw;
                        const float kj = kp[j];
                        for (int x = 0; x < tw; x++)
                            dst[x] += kj * src[x];
                    }
                    continue;
                }
                std::fill(dst, dst + tw, 0.0f);
                const float* tmp = st.tmp[(size_t)c - 1].data();
                for (int t = (o + u.c) & 1; t < K; t += 2)
                {
                    const int i = clampi(u.raw(o, t), 0, h - 1);
                    const float* src = tmp + (size_t)i * (size_t)tw;
                    const float kt = k[t];
                    for (int x = 0; x < tw; x++)
                        dst[x] += kt * src[x];
                }
            }
        });
    }
}

void Net::upsampleBackward(const float* params, float* grads, int dropFinest)
{
    const int L = m_.arch.latentHi;
    const int K = m_.arch.upsK;
    const int KP = m_.arch.preconcatK;
    const UpTaps u(K);

    // A synthesis grid's gradient onto its latents (none for a dropped one).
    auto toLatents = [&](int s, const float* g, size_t row0, size_t n) {
        if (s < dropFinest)
            return;
        float* gq = gq_.data() + gridOff_[(size_t)m_.synGrid[(size_t)s]] + row0;
        for (size_t i = 0; i < n; i++)
            gq[i] += g[i];
    };
    if (L == 0)
    {
        toLatents(0, gz_[0].data(), 0, gz_[0].size());
        return;
    }
    for (int step = L - 1; step >= 0; step--)
    {
        const int s = L - 1 - step;
        UpStep& st = ups_[(size_t)step];
        const std::vector<std::vector<float>>& gnext =
            step + 1 < L ? ups_[(size_t)step + 1].gin : gz_;
        const int h = st.h, w = st.w, th = st.th, tw = st.tw;
        const int nc = (int)st.in.size();
        float k[16], kp[16];
        expandKernel(params + m_.tensors[(size_t)m_.upsWeight[(size_t)step]].offset, K, k);
        expandKernel(params + m_.tensors[(size_t)m_.preWeight[(size_t)step]].offset, KP, kp);
        std::fill(kernelGrad_.begin(), kernelGrad_.end(), 0.0);
        const int p = KP / 2;

        // The vertical passes' transposes, gathered by input row, and their
        // kernels' gradients.
        pool_.run(th + nc * h, [&](int r0, int r1, int thread) {
            double* tk = kernelGrad_.data() + (size_t)thread * 32;
            double* tkp = tk + 16;
            for (int r = r0; r < r1; r++)
            {
                if (r < th)
                {
                    // The target: input row r was read by output row r - j + p.
                    const float* tmp = st.preTmp.data() + (size_t)r * (size_t)tw;
                    float* dst = st.preGtmp.data() + (size_t)r * (size_t)tw;
                    std::fill(dst, dst + tw, 0.0f);
                    for (int j = 0; j < KP; j++)
                    {
                        const int o = r - j + p;
                        if (o < 0 || o >= th)
                            continue;
                        const float* gy = gnext[0].data() + (size_t)o * (size_t)tw;
                        const float kj = kp[j];
                        for (int x = 0; x < tw; x++)
                            dst[x] += kj * gy[x];
                        tkp[j] += dot(gy, tmp, tw);
                    }
                    continue;
                }
                const int c = (r - th) / h, i = (r - th) % h;
                const float* tmp = st.tmp[(size_t)c].data() + (size_t)i * (size_t)tw;
                float* dst = st.gtmp[(size_t)c].data() + (size_t)i * (size_t)tw;
                std::fill(dst, dst + tw, 0.0f);
                const float* gy = gnext[(size_t)c + 1].data();
                for (int t = 0; t < K; t++)
                {
                    int o0, o1;
                    upReaders(u, t, i, h, th, o0, o1);
                    const float kt = k[t];
                    for (int o = o0; o <= o1; o += 2)
                    {
                        const float* src = gy + (size_t)o * (size_t)tw;
                        for (int x = 0; x < tw; x++)
                            dst[x] += kt * src[x];
                        tk[t] += dot(src, tmp, tw);
                    }
                }
            }
        });
        // The horizontal passes' transposes, row by row; the target's
        // gradient (through the filter and its residual) goes to its latents.
        pool_.run(th + nc * h, [&](int r0, int r1, int thread) {
            float* scratch = rowScratch_[(size_t)thread].data();
            double* tk = kernelGrad_.data() + (size_t)thread * 32;
            double* tkp = tk + 16;
            for (int r = r0; r < r1; r++)
            {
                if (r < th)
                {
                    float* gx = scratch + 2 * (size_t)(tw + 2 * PAD);
                    preRowBack(st.preIn.data() + (size_t)r * (size_t)tw, tw, kp, KP,
                               st.preGtmp.data() + (size_t)r * (size_t)tw, gx, tkp, scratch);
                    const float* gy = gnext[0].data() + (size_t)r * (size_t)tw;
                    for (int x = 0; x < tw; x++)
                        gx[x] += gy[x];
                    toLatents(s, gx, (size_t)r * (size_t)tw, (size_t)tw);
                    continue;
                }
                const int c = (r - th) / h, y = (r - th) % h;
                upRowBack(st.in[(size_t)c].data() + (size_t)y * (size_t)w, w, k, u,
                          st.gtmp[(size_t)c].data() + (size_t)y * (size_t)tw, tw,
                          st.gin[(size_t)c].data() + (size_t)y * (size_t)w, tk, scratch);
            }
        });

        double fk[16] = {0}, fkp[16] = {0};
        for (int t = 0; t < threads_; t++)
            for (int j = 0; j < 16; j++)
            {
                fk[j] += kernelGrad_[(size_t)t * 32 + (size_t)j];
                fkp[j] += kernelGrad_[(size_t)t * 32 + 16 + (size_t)j];
            }
        foldKernelGrad(fk, K, grads + m_.tensors[(size_t)m_.upsWeight[(size_t)step]].offset);
        foldKernelGrad(fkp, KP, grads + m_.tensors[(size_t)m_.preWeight[(size_t)step]].offset);
    }
    // What is left is the coarsest grid's gradient.
    toLatents(L, ups_[0].gin[0].data(), 0, ups_[0].gin[0].size());
}

double Net::synthesisForward(const float* params, const Target& target, bool roundOutput,
                             bool wantGrad)
{
    const int H = m_.height;
    const int W = m_.width;
    const size_t P = (size_t)H * (size_t)W;
    const int nOut = m_.arch.synOutputs();
    act_.resize(m_.synLayers.size());
    const std::vector<std::vector<float>>* in = &z_;
    for (size_t li = 0; li < m_.synLayers.size(); li++)
    {
        const ConvRef& c = m_.synLayers[li];
        const float* Wt = params + m_.tensors[(size_t)c.weight].offset;
        const float* B = params + m_.tensors[(size_t)c.bias].offset;
        std::vector<std::vector<float>>& out = act_[li];
        out.assign((size_t)c.layer.out, std::vector<float>(P));
        const std::vector<std::vector<float>>& src = *in;
        const int kk = c.layer.k;
        pool_.run(H, [&](int r0, int r1, int) {
            for (int y = r0; y < r1; y++)
                for (int o = 0; o < c.layer.out; o++)
                {
                    float* dst = out[(size_t)o].data() + (size_t)y * (size_t)W;
                    std::fill(dst, dst + W, B[o]);
                    for (int i = 0; i < c.in; i++)
                    {
                        if (kk == 1)
                        {
                            const float w = Wt[o * c.in + i];
                            const float* s = src[(size_t)i].data() + (size_t)y * (size_t)W;
                            for (int x = 0; x < W; x++)
                                dst[x] += w * s[x];
                            continue;
                        }
                        // 3 x 3, replicated edges.
                        for (int dy = 0; dy < 3; dy++)
                        {
                            const int yy = clampi(y + dy - 1, 0, H - 1);
                            const float* s = src[(size_t)i].data() + (size_t)yy * (size_t)W;
                            const float* wr = Wt + ((size_t)(o * c.in + i) * 3 + (size_t)dy) * 3;
                            for (int dx = 0; dx < 3; dx++)
                            {
                                const float w = wr[dx];
                                const int off = dx - 1;
                                const int x0 = std::max(0, -off);
                                const int x1 = std::min(W, W - off);
                                for (int x = x0; x < x1; x++)
                                    dst[x] += w * s[x + off];
                                for (int x = 0; x < x0; x++)
                                    dst[x] += w * s[clampi(x + off, 0, W - 1)];
                                for (int x = x1; x < W; x++)
                                    dst[x] += w * s[clampi(x + off, 0, W - 1)];
                            }
                        }
                    }
                    if (c.layer.residual)
                    {
                        const float* s = src[(size_t)o].data() + (size_t)y * (size_t)W;
                        for (int x = 0; x < W; x++)
                            dst[x] += s[x];
                    }
                    if (c.layer.relu)
                        for (int x = 0; x < W; x++)
                            dst[x] = dst[x] > 0.0f ? dst[x] : 0.0f;
                }
        });
        in = &out;
    }
    // The stabiliser (1 x 1 on the inputs) beside the main branch.
    const std::vector<std::vector<float>>& last = act_.back();
    raw_.assign((size_t)nOut * P, 0.0f);
    picture_.assign((size_t)nOut * P, 0.0f);
    if (wantGrad)
        gout_.assign((size_t)nOut * P, 0.0f);
    const float* SW = m_.synStabiliser.weight >= 0
                          ? params + m_.tensors[(size_t)m_.synStabiliser.weight].offset
                          : nullptr;
    const float* SB = m_.synStabiliser.weight >= 0
                          ? params + m_.tensors[(size_t)m_.synStabiliser.bias].offset
                          : nullptr;
    const int nIn = m_.arch.synInputs();
    std::fill(threadSum_.begin(), threadSum_.end(), 0.0);
    const double scale = 2.0 / ((double)nOut * (double)P);
    // The output transform (1 x 1, nOut -> nOut) after them: the identity
    // for RGB, but sent with the synthesis, so its quantised values count.
    const float* OW = params + m_.tensors[(size_t)m_.synOutput.weight].offset;
    const float* OB = params + m_.tensors[(size_t)m_.synOutput.bias].offset;
    pool_.run(H, [&](int r0, int r1, int thread) {
        double sum = 0.0;
        std::vector<float> post((size_t)nOut * (size_t)W);
        for (int y = r0; y < r1; y++)
        {
            for (int o = 0; o < nOut; o++)
            {
                const size_t row = (size_t)o * P + (size_t)y * (size_t)W;
                float* raw = raw_.data() + row;
                const float* a = last[(size_t)o].data() + (size_t)y * (size_t)W;
                std::copy(a, a + W, raw);
                if (SW)
                {
                    for (int x = 0; x < W; x++)
                        raw[x] += SB[o];
                    for (int i = 0; i < nIn; i++)
                    {
                        const float w = SW[o * nIn + i];
                        const float* s = z_[(size_t)i].data() + (size_t)y * (size_t)W;
                        for (int x = 0; x < W; x++)
                            raw[x] += w * s[x];
                    }
                }
            }
            for (int o = 0; o < nOut; o++)
            {
                float* dst = post.data() + (size_t)o * (size_t)W;
                std::fill(dst, dst + W, OB[o]);
                for (int c = 0; c < nOut; c++)
                {
                    const float w = OW[o * nOut + c];
                    const float* src = raw_.data() + (size_t)c * P + (size_t)y * (size_t)W;
                    for (int x = 0; x < W; x++)
                        dst[x] += w * src[x];
                }
            }
            for (int o = 0; o < nOut; o++)
            {
                const size_t row = (size_t)o * P + (size_t)y * (size_t)W;
                const float* v0 = post.data() + (size_t)o * (size_t)W;
                const float* t = target.rgb.data() + row;
                float* pic = picture_.data() + row;
                for (int x = 0; x < W; x++)
                {
                    float v = std::min(std::max(v0[x], 0.0f), 1.0f);
                    if (roundOutput)
                        v = std::nearbyint(v * 255.0f) / 255.0f;
                    pic[x] = v;
                    const double d = (double)v - (double)t[x];
                    sum += d * d;
                    if (wantGrad)
                        gout_[row + (size_t)x] =
                            (v0[x] >= 0.0f && v0[x] <= 1.0f) ? (float)(scale * d) : 0.0f;
                }
            }
            if (!wantGrad)
                continue;
            // Back through the output transform: gout_ becomes the gradient
            // of the sum of the main branch and the stabiliser.
            for (int x = 0; x < W; x++)
            {
                float g[64];
                const size_t at = (size_t)y * (size_t)W + (size_t)x;
                for (int o = 0; o < nOut; o++)
                    g[o] = gout_[(size_t)o * P + at];
                for (int c = 0; c < nOut; c++)
                {
                    float acc = 0.0f;
                    for (int o = 0; o < nOut; o++)
                        acc += OW[o * nOut + c] * g[o];
                    gout_[(size_t)c * P + at] = acc;
                }
            }
        }
        threadSum_[(size_t)thread] += sum;
    });
    double total = 0.0;
    for (double s : threadSum_)
        total += s;
    return total / ((double)nOut * (double)P);
}

void Net::synthesisBackward(const float* params)
{
    const int H = m_.height;
    const int W = m_.width;
    const size_t P = (size_t)H * (size_t)W;
    const int nOut = m_.arch.synOutputs();
    const int nIn = m_.arch.synInputs();
    for (auto& tg : threadGrad_)
        std::fill(tg.begin(), tg.end(), 0.0f);
    if (gact_.size() != act_.size())
    {
        gact_.resize(act_.size());
        for (size_t li = 0; li < act_.size(); li++)
            gact_[li].assign(act_[li].size(), std::vector<float>(P, 0.0f));
    }

    // The stabiliser sets gz_ (or zeroes it).
    const float* SW = m_.synStabiliser.weight >= 0
                          ? params + m_.tensors[(size_t)m_.synStabiliser.weight].offset
                          : nullptr;
    const size_t sgw =
        SW ? m_.tensors[(size_t)m_.synStabiliser.weight].offset - netOffset_ : 0;
    const size_t sgb = SW ? m_.tensors[(size_t)m_.synStabiliser.bias].offset - netOffset_ : 0;
    std::vector<std::vector<float>>& glast = gact_.back();
    pool_.run(H, [&](int r0, int r1, int thread) {
        float* tg = threadGrad_[(size_t)thread].data();
        for (int y = r0; y < r1; y++)
        {
            const size_t row = (size_t)y * (size_t)W;
            for (int i = 0; i < nIn; i++)
                std::fill(gz_[(size_t)i].begin() + (long)row,
                          gz_[(size_t)i].begin() + (long)(row + (size_t)W), 0.0f);
            for (int o = 0; o < nOut; o++)
            {
                const float* go = gout_.data() + (size_t)o * P + row;
                std::copy(go, go + W, glast[(size_t)o].data() + row);
                if (!SW)
                    continue;
                tg[sgb + (size_t)o] += sum(go, W);
                for (int i = 0; i < nIn; i++)
                {
                    const float* s = z_[(size_t)i].data() + row;
                    float* gzi = gz_[(size_t)i].data() + row;
                    const float w = SW[o * nIn + i];
                    tg[sgw + (size_t)(o * nIn + i)] += dot(go, s, W);
                    for (int x = 0; x < W; x++)
                        gzi[x] += w * go[x];
                }
            }
        }
    });

    // The main branch, last layer first. Each layer's input gradient goes
    // into the layer before's output gradient (gact_), the first's into gz_.
    for (int li = (int)m_.synLayers.size() - 1; li >= 0; li--)
    {
        const ConvRef& c = m_.synLayers[(size_t)li];
        const float* Wt = params + m_.tensors[(size_t)c.weight].offset;
        const size_t gw = m_.tensors[(size_t)c.weight].offset - netOffset_;
        const size_t gb = m_.tensors[(size_t)c.bias].offset - netOffset_;
        const std::vector<std::vector<float>>& out = act_[(size_t)li];
        const std::vector<std::vector<float>>& src = li > 0 ? act_[(size_t)li - 1] : z_;
        std::vector<std::vector<float>>& g = gact_[(size_t)li];
        std::vector<std::vector<float>>& gin = li > 0 ? gact_[(size_t)li - 1] : gz_;
        // Through the ReLU.
        if (c.layer.relu)
            pool_.run(H, [&](int r0, int r1, int) {
                for (int o = 0; o < c.layer.out; o++)
                {
                    const float* a = out[(size_t)o].data();
                    float* go = g[(size_t)o].data();
                    for (size_t p = (size_t)r0 * (size_t)W; p < (size_t)r1 * (size_t)W; p++)
                        go[p] = a[p] > 0.0f ? go[p] : 0.0f;
                }
            });
        const int kk = c.layer.k;
        pool_.run(H, [&](int r0, int r1, int thread) {
            float* tg = threadGrad_[(size_t)thread].data();
            for (int y = r0; y < r1; y++)
            {
                const size_t row = (size_t)y * (size_t)W;
                if (li > 0)
                    for (int i = 0; i < c.in; i++)
                        std::fill(gin[(size_t)i].begin() + (long)row,
                                  gin[(size_t)i].begin() + (long)(row + (size_t)W), 0.0f);
                for (int o = 0; o < c.layer.out; o++)
                {
                    const float* go = g[(size_t)o].data() + row;
                    tg[gb + (size_t)o] += sum(go, W);
                    for (int i = 0; i < c.in; i++)
                    {
                        if (kk == 1)
                        {
                            const float* s = src[(size_t)i].data() + row;
                            float* gi = gin[(size_t)i].data() + row;
                            const float w = Wt[o * c.in + i];
                            tg[gw + (size_t)(o * c.in + i)] += dot(go, s, W);
                            for (int x = 0; x < W; x++)
                                gi[x] += w * go[x];
                            continue;
                        }
                        // 3 x 3: the weight gradient here, by output row,
                        // the edge pixels reading themselves again.
                        float* gwr = tg + gw + (size_t)(o * c.in + i) * 9;
                        for (int dy = 0; dy < 3; dy++)
                        {
                            const int yy = clampi(y + dy - 1, 0, H - 1);
                            const float* s = src[(size_t)i].data() + (size_t)yy * (size_t)W;
                            gwr[dy * 3 + 0] += go[0] * s[0] + dot(go + 1, s, W - 1);
                            gwr[dy * 3 + 1] += dot(go, s, W);
                            gwr[dy * 3 + 2] += dot(go, s + 1, W - 1) + go[W - 1] * s[W - 1];
                        }
                    }
                }
                if (kk == 3)
                {
                    // 3 x 3: the input gradient of row y, gathered from the
                    // output rows that read it.
                    for (int i = 0; i < c.in; i++)
                    {
                        float* gi = gin[(size_t)i].data() + row;
                        for (int yo = std::max(0, y - 1); yo <= std::min(H - 1, y + 1); yo++)
                            for (int dy = 0; dy < 3; dy++)
                            {
                                if (clampi(yo + dy - 1, 0, H - 1) != y)
                                    continue;
                                for (int o = 0; o < c.layer.out; o++)
                                {
                                    const float* go =
                                        g[(size_t)o].data() + (size_t)yo * (size_t)W;
                                    const float* wr =
                                        Wt + ((size_t)(o * c.in + i) * 3 + (size_t)dy) * 3;
                                    // dx = 0 reads x - 1, dx = 2 reads x + 1.
                                    gi[0] += wr[0] * go[0];
                                    for (int x = 1; x < W; x++)
                                        gi[x - 1] += wr[0] * go[x];
                                    for (int x = 0; x < W; x++)
                                        gi[x] += wr[1] * go[x];
                                    for (int x = 0; x + 1 < W; x++)
                                        gi[x + 1] += wr[2] * go[x];
                                    gi[W - 1] += wr[2] * go[W - 1];
                                }
                            }
                    }
                }
                if (c.layer.residual)
                    for (int o = 0; o < c.layer.out; o++)
                    {
                        const float* go = g[(size_t)o].data() + row;
                        float* gi = gin[(size_t)o].data() + row;
                        for (int x = 0; x < W; x++)
                            gi[x] += go[x];
                    }
            }
        });
    }
}

void Net::reduceThreadGrads(float* grads)
{
    const size_t n = m_.count - netOffset_;
    for (int t = 0; t < threads_; t++)
    {
        const float* tg = threadGrad_[(size_t)t].data();
        for (size_t i = 0; i < n; i++)
            grads[netOffset_ + i] += tg[i];
    }
}

StepResult Net::step(const float* params, float* grads, const StepConfig& config,
                     const Target& target)
{
    const bool wantGrad = grads != nullptr;
    const double pixels = (double)m_.height * (double)m_.width;
    const double rateWeight = config.lambda / pixels;

    auto t0 = std::chrono::steady_clock::now();
    auto tick = [&](int phase) {
        const auto t1 = std::chrono::steady_clock::now();
        if (times.size() < 10)
            times.assign(10, 0.0);
        times[(size_t)phase] += std::chrono::duration<double, std::milli>(t1 - t0).count();
        t0 = t1;
    };
    quantise(params, config, wantGrad);
    tick(9);
    ifceForward(params);
    if (wantGrad)
        for (auto& tg : threadGrad_)
            std::fill(tg.begin(), tg.end(), 0.0f);
    std::fill(gq_.begin(), gq_.end(), 0.0f);
    tick(0);
    StepResult r;
    r.rateBits = armPass(params, rateWeight, wantGrad);
    tick(1);
    upsampleForward(params, config.dropFinest);
    tick(2);
    r.mse = synthesisForward(params, target, config.roundOutput, wantGrad);
    tick(3);
    r.loss = r.mse + rateWeight * r.rateBits;
    if (!wantGrad)
        return r;

    std::fill(grads, grads + m_.count, 0.0f);
    // The ARM's weight gradients were gathered per thread in armPass; the
    // synthesis' join them there, then all are added up.
    std::vector<std::vector<float>> armGrads = threadGrad_;
    synthesisBackward(params);
    tick(4);
    for (int t = 0; t < threads_; t++)
        for (size_t i = 0; i < armGrads[(size_t)t].size(); i++)
            threadGrad_[(size_t)t][i] += armGrads[(size_t)t][i];
    reduceThreadGrads(grads);
    upsampleBackward(params, grads, config.dropFinest);
    tick(5);
    contextBackward();
    tick(6);
    ifceBackward(params, grads);
    tick(7);
    // Back through the quantiser onto the latents.
    for (size_t i = 0; i < q_.size(); i++)
        grads[i] = gq_[i] * dq_[i];
    tick(8);
    return r;
}

double Net::rate(const float* params)
{
    ifceForward(params);
    return armPass(params, 0.0, false);
}

double Net::synthesise(const float* params, const Target& target, bool roundOutput,
                       int dropFinest)
{
    upsampleForward(params, dropFinest);
    return synthesisForward(params, target, roundOutput, false);
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
