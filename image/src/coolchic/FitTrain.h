// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/training/train.py, warmup.py and presets.py's PresetIntra).
// Authors: see Cool-chic's CONTRIBUTORS.md.
//
// The fit's schedule: a warm-up race between a few random starts, then a
// long phase with soft rounding and noise under a cosine learning rate,
// then a short one with hard rounding. Each phase keeps the best model its
// validations (every 100 steps, hard rounded, as the decoder will see it)
// have found.
//
// Progressive fitting (Ragtime's, after tools/codecs/cc_encode_prog.py): on
// a share of the gradient steps the finest synthesis grids are taken as
// zero, so the picture from the coarse grids alone, all a receiver has of a
// cut-off stream, is a fair one too. The rate is always the whole stream's.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_TRAIN_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_TRAIN_H

#include <cstdint>
#include <functional>
#include <vector>

#include "FitModel.h"
#include "FitNet.h"
#include "FitPool.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// One phase of training (presets.py's TrainerPhase).
struct Phase
{
    double lr = 0.01;
    int maxItr = 1000;
    int freqValid = 100;
    int patience = 10000;
    bool scheduleLr = false;            // cosine, stepped at each validation
    float temperature[2] = {0.3f, 0.3f};    // soft-round, from start to end
    float noise[2] = {0.0f, 0.0f};      // Gaussian noise, from start to end
    bool hardRound = false;             // no noise, no gradient to the latents
    double latentBetas[2] = {0.9, 0.999};
    int preconditionFrequency = 10;
};

// PresetIntra for iterations (cc_encode.py's --n_itr, 2000 or more) at
// lambda: the warm-up's two stages (5 candidates, then 2) and the two
// training phases.
struct Schedule
{
    int candidates[2] = {5, 2};
    Phase warmup;
    std::vector<Phase> phases;
    int totalSteps() const;
};
Schedule intraSchedule(int iterations, double lambda);

struct Progressive
{
    double share = 0.5;     // of the gradient steps
    int maxDropped = 3;     // the finest 1 to maxDropped grids, evenly
};

// Called after every step with the share of the fit done; false cancels.
using FitProgress = std::function<bool(double done)>;

class Trainer
{
public:
    Trainer(const Model& model, const Target& target, Pool& pool, double lambda,
            const Progressive& progressive, uint64_t seed);

    // The whole fit from Cool-chic's random starts; false if cancelled.
    bool fit(const Schedule& schedule, std::vector<float>& params, const FitProgress& progress);

    // One phase on params; false if cancelled (params then as they were
    // at the best validation so far).
    bool train(const Phase& phase, std::vector<float>& params);

    // The loss the decoder's picture scores (hard rounding, 8-bit output).
    StepResult test(const std::vector<float>& params);

    Net& net() { return net_; }

    // Each validation's numbers, for the developer's tool.
    std::function<void(int step, const StepResult& r)> onValidation;

private:
    bool tick();

    const Model& m_;
    const Target& target_;
    Net net_;
    const double lambda_;
    const Progressive progressive_;
    Random rng_;
    uint64_t steps_ = 0;
    int total_ = 1;
    const FitProgress* progress_ = nullptr;
    std::vector<float> grads_;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
