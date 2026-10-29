// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (coolchic/nnquant/: quantizemodel.py, rdoq.py, quantstep.py,
// expgolomb.py). Authors: see Cool-chic's CONTRIBUTORS.md.
//
// After the fit, the networks' weights are quantised for the stream: for
// each network a step for its weights and one for its biases (powers of 2)
// and the exp-Golomb orders they are coded with, picked by trying them all
// on the picture; then each quantised value is nudged up or down a few
// steps where that lowers the score (RDOQ).
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_QUANT_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_QUANT_H

#include <cstdint>
#include <vector>

#include "FitModel.h"
#include "FitNet.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// The four networks in the order Cool-chic writes them.
enum NetIndex
{
    NET_ARM = 0,
    NET_IFCE = 1,
    NET_UPSAMPLING = 2,
    NET_SYNTHESIS = 3,
    NETS = 4
};

// One network's tensors in Cool-chic's parameter order (get_param()):
// weights and biases, each in a list of its own.
struct NetTensors
{
    std::vector<int> weights;
    std::vector<int> biases;
};
NetTensors netTensors(const Model& model, int net);

struct NetQuant
{
    bool done = false;
    int weightShift = 0;    // the step is 2^shift
    int biasShift = 0;
    int weightOrder = 0;    // exp-Golomb orders
    int biasOrder = 0;
};

// Bits of exp-Golomb order k for integer values (coolchic.py's
// exp_golomb_nbins(): within 0.5% of the real code).
double expGolombBits(const std::vector<double>& values, int k);

class Quantiser
{
public:
    Quantiser(const Model& model, const Target& target, Net& net, double lambda);

    // quantize_model(): every network's steps and orders; params are set to
    // their quantised values.
    void quantise(std::vector<float>& params);

    // rdoq_model(): the greedy nudges, in a random order from seed.
    void rdoq(std::vector<float>& params, uint64_t seed);

    // The score the decoder's picture gets, the networks' bits included.
    StepResult test(const std::vector<float>& params);
    double networkBits(const std::vector<float>& params) const;

    const NetQuant& quant(int net) const { return quant_[net]; }

    // A network's values as sent: round(value / step).
    std::vector<int32_t> sent(const std::vector<float>& params, int net, bool biases) const;

private:
    double netBits(const std::vector<float>& params, int net, const NetQuant& q) const;

    const Model& m_;
    const Target& target_;
    Net& net_;
    const double lambda_;
    const double pixels_;
    NetTensors tensors_[NETS];
    NetQuant quant_[NETS];
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
