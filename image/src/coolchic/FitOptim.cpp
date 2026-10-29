// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/training/soap.py, itself from https://github.com/nikhilvyas/SOAP,
// Copyright (c) 2024 Nikhil Vyas, MIT licence: LICENSE-SOAP.txt beside this
// file). Authors: see Cool-chic's CONTRIBUTORS.md.
#include "FitOptim.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
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

using Matrix = std::vector<double>;     // n x n, row-major

// Along one axis of a row-major tensor: v[.., i, ..] = sum_j v[.., j, ..] M,
// with M[j][i] (transposed, soap.py's project()) or M[i][j] (project_back()).
void modeProduct(std::vector<double>& v, const std::vector<int>& dims, size_t axis,
                 const Matrix& M, bool transposed)
{
    const size_t n = (size_t)dims[axis];
    size_t inner = 1;
    for (size_t a = axis + 1; a < dims.size(); a++)
        inner *= (size_t)dims[a];
    const size_t outer = v.size() / (n * inner);
    std::vector<double> col(n), res(n);
    for (size_t o = 0; o < outer; o++)
        for (size_t in = 0; in < inner; in++)
        {
            const size_t base = o * n * inner + in;
            for (size_t j = 0; j < n; j++)
                col[j] = v[base + j * inner];
            for (size_t i = 0; i < n; i++)
            {
                double acc = 0.0;
                for (size_t j = 0; j < n; j++)
                    acc += col[j] * (transposed ? M[j * n + i] : M[i * n + j]);
                res[i] = acc;
            }
            for (size_t i = 0; i < n; i++)
                v[base + i * inner] = res[i];
        }
}

// The Gram matrix of a tensor along one axis: sum over the other axes of
// g[.., i, ..] g[.., j, ..].
Matrix gram(const std::vector<double>& g, const std::vector<int>& dims, size_t axis)
{
    const size_t n = (size_t)dims[axis];
    size_t inner = 1;
    for (size_t a = axis + 1; a < dims.size(); a++)
        inner *= (size_t)dims[a];
    const size_t outer = g.size() / (n * inner);
    Matrix out(n * n, 0.0);
    for (size_t o = 0; o < outer; o++)
        for (size_t in = 0; in < inner; in++)
        {
            const size_t base = o * n * inner + in;
            for (size_t i = 0; i < n; i++)
                for (size_t j = 0; j < n; j++)
                    out[i * n + j] += g[base + i * inner] * g[base + j * inner];
        }
    return out;
}

// Eigenvectors of a symmetric matrix by cyclic Jacobi rotations, as the
// columns of the result, largest eigenvalue first (torch.linalg.eigh,
// flipped).
Matrix eigenvectors(Matrix a, size_t n)
{
    Matrix v(n * n, 0.0);
    for (size_t i = 0; i < n; i++)
        v[i * n + i] = 1.0;
    for (int sweep = 0; sweep < 100; sweep++)
    {
        double off = 0.0, total = 0.0;
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < n; j++)
            {
                total += a[i * n + j] * a[i * n + j];
                if (i != j)
                    off += a[i * n + j] * a[i * n + j];
            }
        if (off <= 1e-30 * total || off == 0.0)
            break;
        for (size_t p = 0; p + 1 < n; p++)
            for (size_t q = p + 1; q < n; q++)
            {
                const double apq = a[p * n + q];
                if (apq == 0.0)
                    continue;
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (size_t k = 0; k < n; k++)
                {
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (size_t k = 0; k < n; k++)
                {
                    const double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (size_t k = 0; k < n; k++)
                {
                    const double vkp = v[k * n + p], vkq = v[k * n + q];
                    v[k * n + p] = c * vkp - s * vkq;
                    v[k * n + q] = s * vkp + c * vkq;
                }
            }
    }
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t x, size_t y) { return a[x * n + x] > a[y * n + y]; });
    Matrix out(n * n);
    for (size_t k = 0; k < n; k++)
        for (size_t i = 0; i < n; i++)
            out[i * n + k] = v[i * n + order[k]];
    return out;
}

// The orthogonal factor of a QR decomposition, by Householder reflections
// (torch.linalg.qr's Q, up to the signs of its columns).
Matrix orthogonalFactor(Matrix a, size_t n)
{
    Matrix q(n * n, 0.0);
    for (size_t i = 0; i < n; i++)
        q[i * n + i] = 1.0;
    std::vector<double> u(n);
    for (size_t k = 0; k + 1 < n; k++)
    {
        double norm = 0.0;
        for (size_t i = k; i < n; i++)
            norm += a[i * n + k] * a[i * n + k];
        norm = std::sqrt(norm);
        double tail = 0.0;
        for (size_t i = k + 1; i < n; i++)
            tail += a[i * n + k] * a[i * n + k];
        if (tail == 0.0)
            continue;
        const double alpha = a[k * n + k] > 0.0 ? -norm : norm;
        for (size_t i = 0; i < n; i++)
            u[i] = i < k ? 0.0 : a[i * n + k];
        u[k] -= alpha;
        double uu = 0.0;
        for (size_t i = k; i < n; i++)
            uu += u[i] * u[i];
        // a = (I - 2 u u^T / u^T u) a, and q = q (I - 2 u u^T / u^T u).
        for (size_t j = 0; j < n; j++)
        {
            double d = 0.0;
            for (size_t i = k; i < n; i++)
                d += u[i] * a[i * n + j];
            d *= 2.0 / uu;
            for (size_t i = k; i < n; i++)
                a[i * n + j] -= d * u[i];
        }
        for (size_t i = 0; i < n; i++)
        {
            double d = 0.0;
            for (size_t j = k; j < n; j++)
                d += q[i * n + j] * u[j];
            d *= 2.0 / uu;
            for (size_t j = k; j < n; j++)
                q[i * n + j] -= d * u[j];
        }
    }
    return q;
}

} // namespace

Optimizer::Optimizer(const Model& model, const OptimConfig& config) : m_(model), config_(config)
{
    for (size_t t = 0; t < m_.tensors.size(); t++)
    {
        const Tensor& tensor = m_.tensors[t];
        if (!tensor.trained)
            continue;
        State s;
        s.tensor = (int)t;
        s.latent = tensor.module == Module::Latent;
        s.dims = tensor.shape;
        s.gg.resize(s.dims.size());
        s.q.resize(s.dims.size());
        // The latents' group has max_precond_dim 0, and vectors aren't
        // preconditioned (precondition_1d false).
        if (!s.latent && s.dims.size() > 1)
            for (size_t a = 0; a < s.dims.size(); a++)
                if (s.dims[a] <= config_.maxPreconditionDim)
                    s.gg[a].assign((size_t)s.dims[a] * (size_t)s.dims[a], 0.0);
        states_.push_back(std::move(s));
    }
}

void Optimizer::updatePreconditioner(State& s, const std::vector<double>& g)
{
    bool any = false;
    for (const Matrix& m : s.gg)
        any = any || !m.empty();
    if (!any)
        return;
    const bool haveQ = !s.q.empty() && std::any_of(s.q.begin(), s.q.end(),
                                                   [](const Matrix& m) { return !m.empty(); });
    if (haveQ)
        for (size_t a = 0; a < s.dims.size(); a++)
            if (!s.q[a].empty())
                modeProduct(s.expAvg, s.dims, a, s.q[a], false);
    const double w = 1.0 - config_.modelBeta2;     // shampoo_beta: betas[1]
    for (size_t a = 0; a < s.dims.size(); a++)
    {
        if (s.gg[a].empty())
            continue;
        const Matrix outer = gram(g, s.dims, a);
        for (size_t i = 0; i < outer.size(); i++)
            s.gg[a][i] += w * (outer[i] - s.gg[a][i]);
    }
    if (!haveQ)
    {
        for (size_t a = 0; a < s.dims.size(); a++)
            if (!s.gg[a].empty())
                s.q[a] = eigenvectors(s.gg[a], (size_t)s.dims[a]);
    }
    else if (s.step > 0 && s.step % config_.preconditionFrequency == 0)
    {
        // One round of power iteration, the eigenbasis kept in the order of
        // its estimated eigenvalues, and the second moments reordered with it.
        for (size_t a = 0; a < s.dims.size(); a++)
        {
            if (s.gg[a].empty())
                continue;
            const size_t n = (size_t)s.dims[a];
            const Matrix& m = s.gg[a];
            const Matrix& o = s.q[a];
            std::vector<double> est(n, 0.0);
            for (size_t k = 0; k < n; k++)
                for (size_t i = 0; i < n; i++)
                {
                    double mo = 0.0;
                    for (size_t j = 0; j < n; j++)
                        mo += m[i * n + j] * o[j * n + k];
                    est[k] += o[i * n + k] * mo;
                }
            std::vector<size_t> order(n);
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t x, size_t y) { return est[x] > est[y]; });
            // exp_avg_sq.index_select(a, order)
            size_t inner = 1;
            for (size_t b = a + 1; b < s.dims.size(); b++)
                inner *= (size_t)s.dims[b];
            const size_t outerCount = s.expAvgSq.size() / (n * inner);
            std::vector<double> sorted(s.expAvgSq.size());
            for (size_t ou = 0; ou < outerCount; ou++)
                for (size_t k = 0; k < n; k++)
                    for (size_t in = 0; in < inner; in++)
                        sorted[(ou * n + k) * inner + in] =
                            s.expAvgSq[(ou * n + order[k]) * inner + in];
            s.expAvgSq = std::move(sorted);
            Matrix power(n * n, 0.0);
            for (size_t i = 0; i < n; i++)
                for (size_t k = 0; k < n; k++)
                {
                    double acc = 0.0;
                    for (size_t j = 0; j < n; j++)
                        acc += m[i * n + j] * o[j * n + order[k]];
                    power[i * n + k] = acc;
                }
            s.q[a] = orthogonalFactor(power, n);
        }
    }
    if (s.step > 0)
        for (size_t a = 0; a < s.dims.size(); a++)
            if (!s.q[a].empty())
                modeProduct(s.expAvg, s.dims, a, s.q[a], true);
}

void Optimizer::step(float* params, const float* grads)
{
    for (State& s : states_)
    {
        const Tensor& t = m_.tensors[(size_t)s.tensor];
        float* p = params + t.offset;
        if (!s.started)
        {
            const std::vector<double> g(grads + t.offset, grads + t.offset + t.size);
            // The first step only sets the preconditioner up, so that it never
            // projects the gradient it was made from.
            s.started = true;
            s.expAvg.assign(t.size, 0.0);
            s.expAvgSq.assign(t.size, 0.0);
            updatePreconditioner(s, g);
            continue;
        }
        const double b1 = s.latent ? config_.latentBeta1 : config_.modelBeta1;
        const double b2 = s.latent ? config_.latentBeta2 : config_.modelBeta2;
        const bool plain = std::all_of(s.q.begin(), s.q.end(),
                                       [](const Matrix& m) { return m.empty(); });
        if (plain)
        {
            // Adam (AdamW for the networks' vectors), in place.
            s.step++;
            const double stepSize = config_.lr * std::sqrt(1.0 - std::pow(b2, s.step)) /
                                    (1.0 - std::pow(b1, s.step));
            const double decay = s.latent ? 0.0 : config_.lr * config_.weightDecay;
            const float* gr = grads + t.offset;
            for (size_t i = 0; i < t.size; i++)
            {
                const double gi = gr[i];
                s.expAvg[i] = b1 * s.expAvg[i] + (1.0 - b1) * gi;
                s.expAvgSq[i] = b2 * s.expAvgSq[i] + (1.0 - b2) * gi * gi;
                double v = (double)p[i] -
                           stepSize * s.expAvg[i] / (std::sqrt(s.expAvgSq[i]) + config_.eps);
                v -= decay * v;
                p[i] = (float)v;
            }
            continue;
        }
        const std::vector<double> g(grads + t.offset, grads + t.offset + t.size);
        std::vector<double> gp = g;
        for (size_t a = 0; a < s.dims.size(); a++)
            if (!s.q[a].empty())
                modeProduct(gp, s.dims, a, s.q[a], true);
        s.step++;
        std::vector<double> upd(t.size);
        for (size_t i = 0; i < t.size; i++)
        {
            s.expAvg[i] = b1 * s.expAvg[i] + (1.0 - b1) * gp[i];
            s.expAvgSq[i] = b2 * s.expAvgSq[i] + (1.0 - b2) * gp[i] * gp[i];
            upd[i] = s.expAvg[i] / (std::sqrt(s.expAvgSq[i]) + config_.eps);
        }
        for (size_t a = 0; a < s.dims.size(); a++)
            if (!s.q[a].empty())
                modeProduct(upd, s.dims, a, s.q[a], false);
        const double stepSize = config_.lr * std::sqrt(1.0 - std::pow(b2, s.step)) /
                                (1.0 - std::pow(b1, s.step));
        const double decay = s.latent ? 0.0 : config_.lr * config_.weightDecay;
        for (size_t i = 0; i < t.size; i++)
        {
            double v = (double)p[i] - stepSize * upd[i];
            v -= decay * v;
            p[i] = (float)v;
        }
        updatePreconditioner(s, g);
    }
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
