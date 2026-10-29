// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/component/latent.py and coolchic.py
// (encode_decode_coolchic). The licence is LICENSE.txt beside this file.

#include "Latents.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "RangeCoder.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

constexpr int ARM_MASK = 9;     // the context mask is 9x9

} // namespace

std::vector<int32_t> codingOrder(int h, int w)
{
    std::vector<int32_t> order;
    order.reserve(size_t(h) * size_t(w));
    if (w <= ARM_MASK)
    {
        for (int i = 0; i < h * w; i++)
            order.push_back(i);
        return order;
    }
    const int step = ARM_MASK + 1;
    const int last = (w - 1) + step * (h - 1);
    for (int t = 0; t <= last; t++)
    {
        const int y0 = t < w ? 0 : (t - w) / step + 1;
        for (int y = y0; y < h; y++)
        {
            const int x = t - step * y;
            if (x < 0)
                break;
            if (x < w)
                order.push_back(y * w + x);
        }
    }
    return order;
}

GridModel::GridModel(const Architecture& arch, const std::vector<Grid>& grids,
                     const std::vector<std::vector<int8_t>>& values, size_t i, const FixedArm& arm,
                     const FixedArm& ifce)
    : arm_(arm), height_(grids[i].height), width_(grids[i].width), ifceOutputs_(arch.ifceOutputs),
      offsets_(contextOffsets(arch.spatialContext))
{
    if (arch.ifceOutputs == 0 || ifce.layers.empty())
        return;
    // The coarser grids, finest first, brought to the next coarser grid's
    // size by nearest neighbour (fixed_upsampling), through the IFCE. The
    // coarsest grid has nothing coarser: zeros at its own size.
    const bool coarsest = i + 1 >= grids.size();
    const Grid& base = coarsest ? grids[i] : grids[i + 1];
    ifceWidth_ = base.width;
    const int inputs = ifce.inputs(), outputs = ifce.outputs();
    ifce_.assign(size_t(base.height) * size_t(base.width) * size_t(outputs), 0);
    std::vector<int64_t> x(static_cast<size_t>(inputs)), out(static_cast<size_t>(outputs));
    for (int y = 0; y < base.height; y++)
        for (int c = 0; c < base.width; c++)
        {
            for (int k = 0; k < inputs; k++)
            {
                const size_t j = i + 1 + size_t(k);
                int64_t v = 0;
                if (!coarsest && j < grids.size())
                {
                    const int shift = grids[j].downsampling - base.downsampling;
                    v = values[j][size_t(y >> shift) * size_t(grids[j].width) + size_t(c >> shift)];
                }
                x[size_t(k)] = v;
            }
            armForward(ifce, x.data(), out.data(), 2 * WEIGHT_SHIFT - FRAC_BITS_IFCE, scratch_);
            std::copy(out.begin(), out.end(),
                      ifce_.begin() +
                          std::ptrdiff_t((size_t(y) * size_t(base.width) + size_t(c)) * size_t(outputs)));
        }
}

void GridModel::indices(const int8_t* grid, int y, int x, int& m, int& s)
{
    const int spatial = int(offsets_.size());
    context_.resize(size_t(spatial + ifceOutputs_));
    out_.resize(2);
    for (int k = 0; k < spatial; k++)
    {
        const int yy = y + offsets_[size_t(k)].first, xx = x + offsets_[size_t(k)].second;
        context_[size_t(k)] = (yy >= 0 && yy < height_ && xx >= 0 && xx < width_)
                                  ? grid[size_t(yy) * size_t(width_) + size_t(xx)]
                                  : 0;
    }
    // The IFCE's features, at half this grid's size (nearest neighbour up).
    for (int k = 0; k < ifceOutputs_; k++)
        context_[size_t(spatial + k)] =
            ifce_.empty() ? 0
                          : ifce_[(size_t(y >> 1) * size_t(ifceWidth_) + size_t(x >> 1)) *
                                      size_t(ifceOutputs_) + size_t(k)];
    armForward(arm_, context_.data(), out_.data(), 2 * WEIGHT_SHIFT - FRAC_BITS_MU_SCALE, scratch_);
    // latent.py: less MU_MIN and LOG_SCALE_MIN in fixed point.
    m = clampMu(out_[0] + (int64_t(LATENT_LIMIT) << FRAC_BITS_MU_SCALE));
    s = clampScale(out_[1] + (int64_t(-LOG_SCALE_MIN) << FRAC_BITS_MU_SCALE));
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
