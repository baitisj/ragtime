// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/component/armint.py and coolchic/component/core/arm.py.
// The licence is LICENSE.txt beside this file.

#include "Arm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
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

// Wrapping arithmetic: a hostile stream may overflow, and that must not be
// undefined behaviour. An honest one never comes near.
inline int64_t mul(int64_t a, int64_t b)
{
    return int64_t(uint64_t(a) * uint64_t(b));
}

inline int64_t add(int64_t a, int64_t b)
{
    return int64_t(uint64_t(a) + uint64_t(b));
}

// The quantisation steps an ARM or IFCE may use: weights 2^-8 to 1,
// biases 2^-16 to 1 (nnquant/quantstep.py).
bool armShiftsValid(int weightShift, int biasShift)
{
    return weightShift >= -8 && weightShift <= 0 && biasShift >= -16 && biasShift <= 0;
}

// One layer from the integers sent (armint.py, arm_to_fixed_point_param).
// w is [outputs][inputs] as sent; the last ifceColumns inputs are the
// IFCE's, already X.8, so their weights are 8 bits less.
FixedLinear makeLinear(const int32_t* w, const int32_t* b, int inputs, int outputs,
                       int weightShift, int biasShift, int ifceColumns, bool residual,
                       bool subtractLogShift)
{
    FixedLinear l;
    l.inputs = inputs;
    l.outputs = outputs;
    l.weight.assign(size_t(inputs) * size_t(outputs), 0);
    l.bias.assign(size_t(outputs), 0);
    const int target = WEIGHT_SHIFT;
    for (int o = 0; o < outputs; o++)
        for (int i = 0; i < inputs; i++)
        {
            const bool ifce = i >= inputs - ifceColumns;
            const int shift = target + weightShift - (ifce ? FRAC_BITS_IFCE : 0);
            int64_t v = mul(int64_t(w[size_t(o) * size_t(inputs) + size_t(i)]), int64_t(1) << shift);
            // An N -> N layer's residual connection, folded into its weights.
            if (residual && inputs == outputs && o == i)
                v = add(v, int64_t(1) << (target - (ifce ? FRAC_BITS_IFCE : 0)));
            l.weight[size_t(i) * size_t(outputs) + size_t(o)] = v;
        }
    for (int o = 0; o < outputs; o++)
    {
        int64_t q = b[o];
        // The hard-coded -4 on the log scale, in units of the bias step.
        if (subtractLogShift && o == 1)
            q = add(q, -(int64_t(4) << -biasShift));
        l.bias[size_t(o)] = mul(q, int64_t(1) << (2 * WEIGHT_SHIFT + biasShift));
    }
    return l;
}

} // namespace

void armForward(const FixedArm& arm, const int64_t* x, int64_t* out, int outputShift,
                std::vector<int64_t>& scratch)
{
    const int n = arm.inputs();
    const int m = arm.outputs();
    const size_t width = size_t(std::max(n, m));
    scratch.resize(4 * width);
    int64_t* cur = scratch.data();
    int64_t* next = cur + width;
    int64_t* stab = next + width;

    for (int i = 0; i < n; i++)
        cur[i] = mul(x[i], int64_t(1) << WEIGHT_SHIFT);

    const FixedLinear& s = arm.stabiliser;
    for (int o = 0; o < m; o++)
        stab[o] = s.bias.empty() ? 0 : s.bias[size_t(o)];
    if (!s.weight.empty())
        for (int i = 0; i < n; i++)
            for (int o = 0; o < m; o++)
                stab[o] = add(stab[o], mul(cur[i], s.weight[size_t(i) * size_t(m) + size_t(o)]));

    const size_t layers = arm.layers.size();
    for (size_t k = 0; k < layers; k++)
    {
        const FixedLinear& l = arm.layers[k];
        for (int o = 0; o < l.outputs; o++)
            next[o] = l.bias[size_t(o)];
        for (int i = 0; i < l.inputs; i++)
        {
            const int64_t xi = cur[i];
            const int64_t* w = l.weight.data() + size_t(i) * size_t(l.outputs);
            for (int o = 0; o < l.outputs; o++)
                next[o] = add(next[o], mul(xi, w[o]));
        }
        if (k + 1 < layers)
            for (int o = 0; o < l.outputs; o++)
                next[o] = next[o] < 0 ? 0 : (next[o] >> WEIGHT_SHIFT);
        std::swap(cur, next);
    }
    for (int o = 0; o < m; o++)
        out[o] = shiftDown(add(cur[o], stab[o]), outputShift);
}

bool fixedArm(const Architecture& arch, const QuantisedNetwork& net, FixedArm& out,
              std::string& error)
{
    const int dim = arch.spatialContext + arch.ifceOutputs;
    const std::vector<int32_t>& w = net.weights[ARM];
    const std::vector<int32_t>& b = net.biases[ARM];
    if (w.size() != parameterCount(arch, ARM, false) || b.size() != parameterCount(arch, ARM, true))
        return error = "ARM weights don't fit", false;
    if (!armShiftsValid(net.weightShift[ARM], net.biasShift[ARM]))
        return error = "ARM quantisation step out of range", false;

    FixedArm arm;
    size_t wi = 0, bi = 0;
    const int layers = arch.armHiddenLayers + 1;
    for (int k = 0; k < layers; k++)
    {
        const bool last = k == layers - 1;
        const int outputs = last ? 2 : dim;
        arm.layers.push_back(makeLinear(w.data() + wi, b.data() + bi, dim, outputs,
                                        net.weightShift[ARM], net.biasShift[ARM],
                                        k == 0 ? arch.ifceOutputs : 0, true, last));
        wi += size_t(dim) * size_t(outputs);
        bi += size_t(outputs);
    }
    if (arch.armStabiliser)
        arm.stabiliser = makeLinear(w.data() + wi, b.data() + bi, dim, 2, net.weightShift[ARM],
                                    net.biasShift[ARM], arch.ifceOutputs, false, false);
    else
    {
        arm.stabiliser.inputs = dim;
        arm.stabiliser.outputs = 2;
    }
    out = std::move(arm);
    return true;
}

bool fixedIfce(const Architecture& arch, const QuantisedNetwork& net, std::vector<FixedArm>& out,
               std::string& error)
{
    const std::vector<Grid> g = grids(arch);
    const std::vector<int32_t>& w = net.weights[IFCE];
    const std::vector<int32_t>& b = net.biases[IFCE];
    if (w.size() != parameterCount(arch, IFCE, false) || b.size() != parameterCount(arch, IFCE, true))
        return error = "IFCE weights don't fit", false;
    if (arch.ifceOutputs > 0 && !armShiftsValid(net.weightShift[IFCE], net.biasShift[IFCE]))
        return error = "IFCE quantisation step out of range", false;

    std::vector<FixedArm> ifce(g.size());
    size_t wi = 0, bi = 0;
    for (size_t i = 0; i < g.size(); i++)
    {
        const int inputs = g[i].ifceInputs;
        if (arch.ifceOutputs == 0 || inputs == 0)
            continue;
        // One linear layer: never residual, no -4, no stabiliser.
        ifce[i].layers.push_back(makeLinear(w.data() + wi, b.data() + bi, inputs, arch.ifceOutputs,
                                            net.weightShift[IFCE], net.biasShift[IFCE], 0, false,
                                            false));
        ifce[i].stabiliser.inputs = inputs;
        ifce[i].stabiliser.outputs = arch.ifceOutputs;
        wi += size_t(inputs) * size_t(arch.ifceOutputs);
        bi += size_t(arch.ifceOutputs);
    }
    out = std::move(ifce);
    return true;
}

std::vector<std::pair<int, int>> contextOffsets(int spatialContext)
{
    // arm.py's get_priority_order: the priority of each of the 40 causal
    // positions of the 9x9 mask, row by row; the ARM takes the
    // spatialContext lowest.
    static const int priority[40] = {
        38, 35, 30, 25, 23, 31, 36, 37, 39,
        33, 28, 21, 20, 6,  15, 22, 29, 34,
        32, 18, 12, 10, 5,  9,  14, 19, 27,
        24, 13, 8,  2,  1,  3,  11, 17, 26,
        16, 7,  4,  0,
    };
    std::vector<std::pair<int, int>> offsets;
    for (int p = 0; p < 40 && int(offsets.size()) < spatialContext; p++)
        for (int idx = 0; idx < 40; idx++)
            if (priority[idx] == p)
                offsets.emplace_back(idx / 9 - 4, idx % 9 - 4);
    return offsets;
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
