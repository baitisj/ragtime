// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1: the
// training forward pass of coolchic/component/core/ (coolchic.py, arm.py,
// quantizer.py, upsampling.py, synthesis.py) and the loss of
// coolchic/training/loss.py, with their backward passes written out by
// hand in place of PyTorch's autograd. Authors: see Cool-chic's
// CONTRIBUTORS.md.
//
// One step of the fit: quantise the latents (soft-round with noise while
// fitting, hard rounding to test), measure their rate with the ARM and the
// IFCEs, upsample them, synthesise the picture, and score
// MSE + lambda * bits per pixel; then the gradient of that score with
// respect to every parameter.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_NET_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_NET_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "FitModel.h"
#include "FitPool.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// The picture to fit: planar RGB in [0, 1], [3][height][width].
struct Target
{
    int height = 0;
    int width = 0;
    std::vector<float> rgb;
};

struct StepConfig
{
    bool hardRound = false;     // test: round(), no noise and no gradient through it
    float temperature = 0.3f;   // soft-round temperature
    float noise = 0.0f;         // Gaussian noise's standard deviation
    double lambda = 0.01;
    int dropFinest = 0;         // progressive fitting: finest synthesis grids taken as zero
    bool roundOutput = false;   // test: the picture rounded to 8 bits, as saved
    uint64_t seed = 1;          // the noise
};

struct StepResult
{
    double loss = 0.0;          // mse + lambda * rateBits / pixels
    double mse = 0.0;
    double rateBits = 0.0;      // the latents', from the float ARM
};

// Scratch space for steps, kept between them.
class Net
{
public:
    Net(const Model& model, Pool& pool);

    // The score of params on target; with grads non-null, its gradient too
    // (grads is overwritten, model.count floats).
    StepResult step(const float* params, float* grads, const StepConfig& config,
                    const Target& target);

    // The decoded picture of the last step, [3][height][width], clamped to
    // [0, 1] (and rounded to 8 bits if the step asked for it).
    const std::vector<float>& picture() const { return picture_; }

    // The quantised latents of the last step, all grids, finest first.
    const std::vector<float>& latents() const { return q_; }
    // Milliseconds spent in each part of step(), added up, for profiling.
    std::vector<double> times;
    size_t gridOffset(int grid) const { return gridOff_[(size_t)grid]; }

    // Pieces, for the tests and for the weight quantisation's partial
    // passes. rate(): the ARM and IFCE pass on the latents now in
    // latents() (no gradient). synthesise(): upsampling and synthesis of
    // them into picture(), returning the MSE.
    double rate(const float* params);
    double synthesise(const float* params, const Target& target, bool roundOutput,
                      int dropFinest = 0);
    // Sets the latents from params, hard rounded.
    void quantiseHard(const float* params);

private:
    // One upsampling step: h x w channels in, th x tw out.
    struct UpStep
    {
        int h = 0, w = 0, th = 0, tw = 0;
        std::vector<std::vector<float>> in;     // the state's channels, h x w
        std::vector<std::vector<float>> tmp;    // after the horizontal pass, h x tw
        std::vector<std::vector<float>> gin, gtmp;  // their gradients
        std::vector<float> preIn;               // the target grid, th x tw
        std::vector<float> preTmp, preGtmp;
    };

    static constexpr int ARM_BLOCK = 64;    // latents through the ARM at once
    struct ArmLinear
    {
        const float* W = nullptr;
        const float* B = nullptr;
        size_t gw = 0, gb = 0;  // offsets into a thread's gradient
    };
    struct ArmLayers
    {
        ArmLinear layer[9];     // the hidden layers, then the output
        ArmLinear stab;
        double rateWeight = 0.0;
    };

    void quantise(const float* params, const StepConfig& config, bool wantGrad);
    double armPass(const float* params, double rateWeight, bool wantGrad);
    template <int DT>
    void armRows(const ArmLayers& a, int r0, int r1, int thread, bool wantGrad);
    void ifceForward(const float* params);
    void ifceBackward(const float* params, float* grads);
    void contextBackward();
    void upsampleForward(const float* params, int dropFinest);
    void upsampleBackward(const float* params, float* grads, int dropFinest);
    double synthesisForward(const float* params, const Target& target, bool roundOutput,
                            bool wantGrad);
    void synthesisBackward(const float* params);
    void reduceThreadGrads(float* grads);

    const Model& m_;
    Pool& pool_;
    const int threads_;
    const size_t netOffset_;    // the first network parameter
    const int dim_;             // ARM inputs

    std::vector<size_t> gridOff_;
    std::vector<int> rowGrid_;  // all grids' rows, back to back: grid and row
    std::vector<int> rowY_;

    std::vector<float> q_;      // quantised latents
    std::vector<float> dq_;     // dq/dy
    std::vector<float> gq_;     // dLoss/dq
    std::vector<float> dctx_;   // dLoss/dcontext, [dim_][latents]
    std::vector<std::vector<float>> armScratch_;    // per thread
    std::vector<std::vector<float>> ifceF_;     // per grid: [out][h'][w'] at the next grid's size
    std::vector<std::vector<float>> threadGrad_;    // per thread: network gradients
    std::vector<double> threadSum_;

    std::vector<UpStep> ups_;
    std::vector<std::vector<float>> rowScratch_;    // per thread
    std::vector<double> kernelGrad_;            // per thread: 16 upsampling, 16 pre
    std::vector<std::vector<float>> z_;         // synthesis inputs, H x W each
    std::vector<std::vector<float>> gz_;
    std::vector<std::vector<std::vector<float>>> act_;  // per layer, per channel
    std::vector<std::vector<std::vector<float>>> gact_; // their gradients
    std::vector<std::vector<float>> stab_;
    std::vector<float> raw_;                    // synthesis output before the clamp
    std::vector<float> picture_;
    std::vector<float> gout_;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
