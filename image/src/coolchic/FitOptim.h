// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/training/train.py's optimiser set-up and coolchic/training/
// soap.py). Authors: see Cool-chic's CONTRIBUTORS.md. soap.py is itself
// from https://github.com/nikhilvyas/SOAP, Copyright (c) 2024 Nikhil Vyas,
// MIT licence (LICENSE-SOAP.txt beside this file).
//
// The optimiser Cool-chic fits with: SOAP (Adam in the eigenbasis of
// Shampoo's preconditioner, https://arxiv.org/abs/2409.11321) on the
// networks' weights, with weight decay, and plain Adam on the latents.
// Both skip their first step, which only sets the preconditioner up.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_OPTIM_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_OPTIM_H

#include <cstddef>
#include <vector>

#include "FitModel.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

struct OptimConfig
{
    double lr = 0.01;
    double modelBeta1 = 0.95, modelBeta2 = 0.95;
    double latentBeta1 = 0.9, latentBeta2 = 0.999;
    int preconditionFrequency = 10;     // the networks' eigenbases' refresh
    double weightDecay = 0.01;          // the networks' only
    double eps = 1e-8;
    int maxPreconditionDim = 256;       // longer dimensions go unpreconditioned
};

class Optimizer
{
public:
    Optimizer(const Model& model, const OptimConfig& config);

    void setLr(double lr) { config_.lr = lr; }
    double lr() const { return config_.lr; }

    // One step on every trained tensor; grads is model.count floats.
    void step(float* params, const float* grads);

private:
    // One tensor's state. A dimension of the tensor has a preconditioner
    // (gg, q: n x n) when it is short enough and the tensor isn't a
    // vector; the others are left as they are.
    struct State
    {
        int tensor = 0;
        bool latent = false;
        std::vector<int> dims;
        std::vector<std::vector<double>> gg, q;     // per dimension; empty: none
        std::vector<double> expAvg, expAvgSq;
        int step = 0;
        bool started = false;
    };

    void updatePreconditioner(State& s, const std::vector<double>& g);

    const Model& m_;
    OptimConfig config_;
    std::vector<State> states_;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
