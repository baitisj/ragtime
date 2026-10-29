// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/component/armint.py, latent.py and coolchic.py, and
// coolchic/component/core/arm.py. The licence is LICENSE.txt beside this
// file.
//
// The ARM and the IFCE in fixed point. Every station must get the same
// probabilities from the same bytes, or the range decoder goes astray, so
// this is integer arithmetic only: 64-bit, wrapping as PyTorch's int64
// does, never undefined on a hostile stream.
#ifndef RAGTIME_IMAGE_COOLCHIC_ARM_H
#define RAGTIME_IMAGE_COOLCHIC_ARM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// Fixed point (armint.py): an input is X.8 or a whole latent, shifted up by
// WEIGHT_SHIFT before the first layer; weights are X.16, biases X.32.
constexpr int WEIGHT_SHIFT = 16;
constexpr int FRAC_BITS_MU_SCALE = 8;       // mu and log scale come out X.8
constexpr int FRAC_BITS_IFCE = 8;           // and the IFCE's features too
constexpr int LOG_SCALE_MIN = -5;           // the log scale's range, before the -4
constexpr int LOG_SCALE_MAX = 5;

// y = W x + b in fixed point, W kept as [inputs][outputs].
struct FixedLinear
{
    int inputs = 0;
    int outputs = 0;
    std::vector<int64_t> weight;
    std::vector<int64_t> bias;
};

// An MLP: hidden layers (relu, residual folded into the weights), an
// output layer and a linear stabiliser beside them (all zero if none).
struct FixedArm
{
    std::vector<FixedLinear> layers;
    FixedLinear stabiliser;
    int inputs() const { return layers.empty() ? 0 : layers.front().inputs; }
    int outputs() const { return layers.empty() ? 0 : layers.back().outputs; }
};

// armint.py's fixed_point_arm: x holds inputs() values, out gets outputs()
// values shifted down by outputShift. scratch is reused between calls.
void armForward(const FixedArm& arm, const int64_t* x, int64_t* out, int outputShift,
                std::vector<int64_t>& scratch);

// The ARM of a decoded network, as arm_to_fixed_point_param makes it
// (the log scale's -4 in the last bias, the IFCE's columns 8 bits less).
// False, with the reason, if the weights don't fit the architecture.
bool fixedArm(const Architecture& arch, const QuantisedNetwork& net, FixedArm& out,
              std::string& error);

// The IFCE, one per grid (empty where the grid has none), in grids(arch)'s
// order.
bool fixedIfce(const Architecture& arch, const QuantisedNetwork& net,
               std::vector<FixedArm>& out, std::string& error);

// The spatial context's neighbours, (dy, dx) from the latent being coded,
// in the order the ARM takes them: Cool-chic's priority order over the
// causal half of a 9x9 mask.
std::vector<std::pair<int, int>> contextOffsets(int spatialContext);

// Arithmetic shift right that floors, as PyTorch's >> does on int64.
inline int64_t shiftDown(int64_t v, int s)
{
    return v >= 0 ? (v >> s) : ~((~v) >> s);
}

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
