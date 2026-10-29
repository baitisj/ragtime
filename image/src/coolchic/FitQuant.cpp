// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/nnquant/). Authors: see Cool-chic's CONTRIBUTORS.md.
#include "FitQuant.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// quantstep.py's POSSIBLE_Q_STEP, as shifts: weights, then biases.
struct StepRange
{
    int wLo, wHi, bLo, bHi;
};
const StepRange STEPS[NETS] = {
    {-8, 0, -16, 0},    // ARM
    {-8, 0, -16, 0},    // IFCE
    {-12, 0, 0, 0},     // upsampling
    {-12, 0, -24, 0},   // synthesis
};
constexpr int MAX_ORDER = 12;               // expgolomb.py: orders 0 to 12
constexpr double MAX_SENT = 65535.0;        // a 16-bit field in the header
constexpr int RDOQ_SHIFTS = 15;

// Cool-chic quantises its networks in the order of their names: arm, ifce,
// synthesis, upsampling.
const int NAME_ORDER[NETS] = {NET_ARM, NET_IFCE, NET_SYNTHESIS, NET_UPSAMPLING};

} // namespace

NetTensors netTensors(const Model& m, int net)
{
    NetTensors t;
    auto add = [&](int w, int b) {
        if (w >= 0)
            t.weights.push_back(w);
        if (b >= 0)
            t.biases.push_back(b);
    };
    switch (net)
    {
    case NET_ARM:
        for (const LinearRef& l : m.armLayers)
            add(l.weight, l.bias);
        add(m.armStabiliser.weight, m.armStabiliser.bias);
        break;
    case NET_IFCE:
        for (const LinearRef& l : m.ifce)
            add(l.weight, l.bias);
        break;
    case NET_UPSAMPLING:
        for (size_t i = 0; i < m.upsWeight.size(); i++)
            add(m.upsWeight[i], m.upsBias[i]);
        for (size_t i = 0; i < m.preWeight.size(); i++)
            add(m.preWeight[i], m.preBias[i]);
        break;
    case NET_SYNTHESIS:
        add(m.synOutput.weight, m.synOutput.bias);
        add(m.synStabiliser.weight, m.synStabiliser.bias);
        for (const ConvRef& c : m.synLayers)
            add(c.weight, c.bias);
        break;
    default:
        break;
    }
    return t;
}

double expGolombBits(const std::vector<double>& values, int k)
{
    const double scale = std::ldexp(2.0, -k);
    double bits = 0.0;
    for (double v : values)
        bits += 2.0 * std::floor(std::log2(std::fabs(v) * scale + 1.0)) + k + (v != 0.0 ? 1 : 0);
    return bits;
}

Quantiser::Quantiser(const Model& model, const Target& target, Net& net, double lambda)
    : m_(model),
      target_(target),
      net_(net),
      lambda_(lambda),
      pixels_((double)model.height * (double)model.width)
{
    for (int n = 0; n < NETS; n++)
        tensors_[n] = netTensors(model, n);
}

std::vector<int32_t> Quantiser::sent(const std::vector<float>& params, int net, bool biases) const
{
    const NetQuant& q = quant_[net];
    const double step = std::ldexp(1.0, biases ? q.biasShift : q.weightShift);
    std::vector<int32_t> out;
    for (int t : biases ? tensors_[net].biases : tensors_[net].weights)
    {
        const Tensor& tensor = m_.tensors[(size_t)t];
        for (size_t i = 0; i < tensor.size; i++)
            out.push_back((int32_t)std::nearbyint(params[tensor.offset + i] / step));
    }
    return out;
}

double Quantiser::netBits(const std::vector<float>& params, int net, const NetQuant& q) const
{
    double bits = 0.0;
    for (int b = 0; b < 2; b++)
    {
        const double step = std::ldexp(1.0, b ? q.biasShift : q.weightShift);
        std::vector<double> v;
        for (int t : b ? tensors_[net].biases : tensors_[net].weights)
        {
            const Tensor& tensor = m_.tensors[(size_t)t];
            for (size_t i = 0; i < tensor.size; i++)
                v.push_back(std::nearbyint(params[tensor.offset + i] / step));
        }
        bits += expGolombBits(v, b ? q.biasOrder : q.weightOrder);
    }
    return bits;
}

double Quantiser::networkBits(const std::vector<float>& params) const
{
    double bits = 0.0;
    for (int n = 0; n < NETS; n++)
        if (quant_[n].done)
            bits += netBits(params, n, quant_[n]);
    return bits;
}

StepResult Quantiser::test(const std::vector<float>& params)
{
    StepResult r;
    net_.quantiseHard(params.data());
    r.rateBits = net_.rate(params.data());
    r.mse = net_.synthesise(params.data(), target_, true);
    r.loss = r.mse + lambda_ * (r.rateBits + networkBits(params)) / pixels_;
    return r;
}

bool Quantiser::quantise(std::vector<float>& params)
{
    net_.quantiseHard(params.data());
    double rate = net_.rate(params.data());
    double mse = net_.synthesise(params.data(), target_, true);
    const std::vector<float> full = params;

    for (int n : NAME_ORDER)
    {
        const NetTensors& nt = tensors_[n];
        if (nt.weights.empty() && nt.biases.empty())
            continue;
        const StepRange& range = STEPS[n];
        double bestLoss = 1e6;
        NetQuant best;
        double bestRate = rate, bestMse = mse;
        double otherBits = networkBits(params);
        std::vector<float> trial = params;
        for (int ws = range.wLo; ws <= range.wHi; ws++)
            for (int bs = range.bLo; bs <= range.bHi; bs++)
            {
                if (going && !going())
                {
                    params = full;
                    return false;
                }
                // Quantise from the full-precision values, unless a value
                // needs more than 16 bits.
                bool ok = true;
                for (int b = 0; b < 2 && ok; b++)
                {
                    const double step = std::ldexp(1.0, b ? bs : ws);
                    for (int t : b ? nt.biases : nt.weights)
                    {
                        const Tensor& tensor = m_.tensors[(size_t)t];
                        for (size_t i = 0; i < tensor.size && ok; i++)
                        {
                            const double s = std::nearbyint(full[tensor.offset + i] / step);
                            if (std::fabs(s) > MAX_SENT)
                                ok = false;
                            trial[tensor.offset + i] = (float)(s * step);
                        }
                    }
                }
                if (!ok)
                    continue;
                NetQuant q;
                q.done = true;
                q.weightShift = ws;
                q.biasShift = bs;
                for (int b = 0; b < 2; b++)
                {
                    const double step = std::ldexp(1.0, b ? bs : ws);
                    std::vector<double> v;
                    for (int t : b ? nt.biases : nt.weights)
                    {
                        const Tensor& tensor = m_.tensors[(size_t)t];
                        for (size_t i = 0; i < tensor.size; i++)
                            v.push_back((double)trial[tensor.offset + i] / step);
                    }
                    double bestBits = 1e18;
                    int bestOrder = 0;
                    for (int k = 0; k <= MAX_ORDER; k++)
                    {
                        const double bits = expGolombBits(v, k);
                        if (bits < bestBits)
                        {
                            bestBits = bits;
                            bestOrder = k;
                        }
                    }
                    (b ? q.biasOrder : q.weightOrder) = bestOrder;
                }
                // Only what this network changes is run again.
                double r = rate, d = mse;
                if (n == NET_ARM || n == NET_IFCE)
                    r = net_.rate(trial.data());
                else
                    d = net_.synthesise(trial.data(), target_, true);
                const double loss =
                    d + lambda_ * (r + otherBits + netBits(trial, n, q)) / pixels_;
                if (loss < bestLoss)
                {
                    bestLoss = loss;
                    best = q;
                    bestRate = r;
                    bestMse = d;
                }
            }
        quant_[n] = best;
        for (int b = 0; b < 2; b++)
        {
            const double step = std::ldexp(1.0, b ? best.biasShift : best.weightShift);
            for (int t : b ? nt.biases : nt.weights)
            {
                const Tensor& tensor = m_.tensors[(size_t)t];
                for (size_t i = 0; i < tensor.size; i++)
                    params[tensor.offset + i] =
                        (float)(std::nearbyint(full[tensor.offset + i] / step) * step);
            }
        }
        rate = bestRate;
        mse = bestMse;
    }
    return true;
}

bool Quantiser::rdoq(std::vector<float>& params, uint64_t seed)
{
    Random rng(seed);
    net_.quantiseHard(params.data());

    for (int n : NAME_ORDER)
    {
        const NetTensors& nt = tensors_[n];
        const NetQuant& q = quant_[n];
        if (!q.done)
            continue;
        const double rate0 = net_.rate(params.data());
        // The score each network's nudges are judged by (rdoq.py's score_*).
        auto score = [&]() {
            switch (n)
            {
            case NET_ARM:
            case NET_IFCE:
                return net_.rate(params.data()) + netBits(params, n, q);
            case NET_SYNTHESIS:
                return net_.synthesise(params.data(), target_, true) +
                       lambda_ * netBits(params, n, q) / pixels_;
            default:
                return net_.synthesise(params.data(), target_, true) +
                       lambda_ * (rate0 + networkBits(params)) / pixels_;
            }
        };
        for (int b = 0; b < 2; b++)
        {
            std::vector<size_t> index;
            for (int t : b ? nt.biases : nt.weights)
            {
                const Tensor& tensor = m_.tensors[(size_t)t];
                for (size_t i = 0; i < tensor.size; i++)
                    index.push_back(tensor.offset + i);
            }
            if (index.empty())
                continue;
            // torch.randperm's part: a random order.
            for (size_t i = index.size() - 1; i > 0; i--)
                std::swap(index[i], index[(size_t)(rng.next() % (i + 1))]);
            const double step = std::ldexp(1.0, b ? q.biasShift : q.weightShift);
            double bestLoss = score();
            for (size_t at : index)
            {
                if (going && !going())
                    return false;
                const float initial = params[at];
                float bestValue = initial;
                for (int sign = -1; sign <= 1; sign += 2)
                    for (int shift = 1; shift <= RDOQ_SHIFTS; shift++)
                    {
                        params[at] = (float)((double)initial + sign * shift * step);
                        const double loss = score();
                        if (loss < bestLoss)
                        {
                            bestLoss = loss;
                            bestValue = params[at];
                        }
                        else
                            break;
                    }
                params[at] = bestValue;
            }
        }
    }
    return true;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
