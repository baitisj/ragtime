// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/training/train.py, warmup.py and presets.py). Authors: see
// Cool-chic's CONTRIBUTORS.md.
#include "FitTrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "FitOptim.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double ETA_MIN = 1e-5;           // CosineAnnealingLR's floor
constexpr double CLIP_NORM = 0.1;          // the networks' gradients, L2

float linear(const float range[2], int itr, int maxItr)
{
    return (float)((double)itr * (double)(range[1] - range[0]) / (double)maxItr + range[0]);
}

} // namespace

int Schedule::totalSteps() const
{
    int n = (candidates[0] + candidates[1]) * warmup.maxItr;
    for (const Phase& p : phases)
        n += p.maxItr;
    return n;
}

Schedule intraSchedule(int iterations, double lambda)
{
    Schedule s;
    iterations = std::max(iterations, 2000);
    const float initNoise = (float)((-0.432 * std::log10(lambda) + 0.747) / 10.0);
    const int steTraining = 500;
    int wuIter = 400;
    if (iterations < 4000)
        wuIter = 50;
    else if (iterations < 9000)
        wuIter = 100;
    else if (iterations < 12000)
        wuIter = 200;

    Phase& w = s.warmup;
    w.lr = 0.01;
    w.maxItr = wuIter;
    w.freqValid = 100;
    w.patience = wuIter;
    w.temperature[0] = w.temperature[1] = 0.35f;
    w.noise[0] = w.noise[1] = initNoise;
    w.latentBetas[0] = 0.725;
    w.latentBetas[1] = 0.97;
    w.preconditionFrequency = 1;

    Phase core;
    core.lr = 0.01;
    core.maxItr = iterations - steTraining - (s.candidates[0] + s.candidates[1]) * wuIter;
    core.patience = 5000;
    core.scheduleLr = true;
    core.temperature[0] = 0.35f;
    core.temperature[1] = 0.08f;
    core.noise[0] = 0.22f;
    core.noise[1] = 0.15f;
    s.phases.push_back(core);

    Phase ste;
    ste.lr = 1e-4;
    ste.maxItr = steTraining;
    ste.scheduleLr = true;
    ste.hardRound = true;
    s.phases.push_back(ste);
    return s;
}

Trainer::Trainer(const Model& model, const Target& target, Pool& pool, double lambda,
                 const Progressive& progressive, uint64_t seed)
    : m_(model),
      target_(target),
      net_(model, pool),
      lambda_(lambda),
      progressive_(progressive),
      rng_(seed),
      grads_(model.count, 0.0f)
{
}

StepResult Trainer::test(const std::vector<float>& params)
{
    StepConfig c;
    c.hardRound = true;
    c.roundOutput = true;
    c.lambda = lambda_;
    return net_.step(params.data(), nullptr, c, target_);
}

bool Trainer::tick()
{
    steps_++;
    if (!progress_ || !*progress_)
        return true;
    return (*progress_)(std::min(1.0, (double)steps_ / (double)total_));
}

bool Trainer::train(const Phase& phase, std::vector<float>& params)
{
    double best = test(params).loss;
    std::vector<float> bestParams = params;

    OptimConfig oc;
    oc.lr = phase.lr;
    oc.latentBeta1 = phase.latentBetas[0];
    oc.latentBeta2 = phase.latentBetas[1];
    oc.preconditionFrequency = phase.preconditionFrequency;
    Optimizer opt(m_, oc);
    const double tMax = (double)phase.maxItr / (double)phase.freqValid;
    int lrStep = 0;

    float temperature = linear(phase.temperature, 0, phase.maxItr);
    float noise = linear(phase.noise, 0, phase.maxItr);
    int cntRecord = 0;
    const size_t netStart = m_.latentCount();
    const int maxDrop = std::min(progressive_.maxDropped, m_.arch.latentHi);

    for (int cnt = 0; cnt < phase.maxItr; cnt++)
    {
        // Patience: with a cosine schedule it reloads the best model and
        // goes on at the present learning rate; without, it ends the phase.
        if (cnt - cntRecord > phase.patience)
        {
            if (!phase.scheduleLr)
                break;
            params = bestParams;
            cntRecord = cnt;
        }

        StepConfig c;
        c.hardRound = phase.hardRound;
        c.temperature = temperature;
        c.noise = phase.hardRound ? 0.0f : noise;
        c.lambda = lambda_;
        c.seed = rng_.next();
        if (maxDrop > 0 && progressive_.share > 0.0 && rng_.uniform() < progressive_.share)
            c.dropFinest = 1 + (int)(rng_.uniform() * maxDrop);
        net_.step(params.data(), grads_.data(), c, target_);

        // Only the networks' gradients are clipped.
        double norm = 0.0;
        for (size_t i = netStart; i < m_.count; i++)
            norm += (double)grads_[i] * grads_[i];
        norm = std::sqrt(norm);
        const double coef = std::min(1.0, CLIP_NORM / (norm + 1e-6));
        if (coef < 1.0)
            for (size_t i = netStart; i < m_.count; i++)
                grads_[i] = (float)(grads_[i] * coef);
        opt.step(params.data(), grads_.data());

        if (!tick())
        {
            params = bestParams;
            return false;
        }

        if ((cnt + 1) % phase.freqValid == 0 || cnt + 1 == phase.maxItr)
        {
            const StepResult r = test(params);
            const double loss = r.loss;
            if (onValidation)
                onValidation(cnt + 1, r);
            if (loss < best)
            {
                best = loss;
                bestParams = params;
                cntRecord = cnt;
            }
            temperature = linear(phase.temperature, cnt, phase.maxItr);
            noise = linear(phase.noise, cnt, phase.maxItr);
            if (phase.scheduleLr)
            {
                lrStep++;
                opt.setLr(ETA_MIN + (phase.lr - ETA_MIN) * (1.0 + std::cos(PI * lrStep / tMax)) /
                                        2.0);
            }
        }
    }
    params = bestParams;
    return true;
}

bool Trainer::fit(const Schedule& schedule, std::vector<float>& params,
                  const FitProgress& progress)
{
    progress_ = &progress;
    total_ = std::max(1, schedule.totalSteps());
    steps_ = 0;

    // The warm-up: every candidate a little, then the best few a little
    // more (warmup.py).
    struct Candidate
    {
        std::vector<float> params;
        double loss = 0.0;
    };
    std::vector<Candidate> cands((size_t)schedule.candidates[0]);
    for (size_t i = 0; i < cands.size(); i++)
        initParams(m_, rng_.next(), cands[i].params);
    for (int stage = 0; stage < 2; stage++)
    {
        const size_t keep = std::min(cands.size(), (size_t)schedule.candidates[stage]);
        cands.resize(keep);
        for (Candidate& c : cands)
        {
            if (!train(schedule.warmup, c.params))
            {
                progress_ = nullptr;
                return false;
            }
            c.loss = test(c.params).loss;
        }
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Candidate& a, const Candidate& b) { return a.loss < b.loss; });
    }
    params = std::move(cands[0].params);

    for (const Phase& p : schedule.phases)
        if (!train(p, params))
        {
            progress_ = nullptr;
            return false;
        }
    progress_ = nullptr;
    return true;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
