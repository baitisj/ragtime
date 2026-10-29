// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/component/latent.py and coolchic.py
// (encode_decode_coolchic). The licence is LICENSE.txt beside this file.
//
// What the decoder and the encoder's writer both need to code one grid's
// latents: the order they go in, and each one's Laplace model from the
// ARM, its causal neighbours and the IFCE's features from coarser grids.
#ifndef RAGTIME_IMAGE_COOLCHIC_LATENTS_H
#define RAGTIME_IMAGE_COOLCHIC_LATENTS_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Arm.h"
#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

// latent.py's wavefront: value (y, x) is coded at step x + 10 y, and within
// a step from the top row down; a grid no wider than the 9x9 mask goes row
// by row. Each entry is y * w + x.
std::vector<int32_t> codingOrder(int h, int w);

// The model for one grid's latents.
class GridModel
{
public:
    // grids: grids(arch); values: every grid's latents (finest first, row
    // by row), of which the grids coarser than index must be whole; ifce:
    // fixedIfce's, for this grid.
    GridModel(const Architecture& arch, const std::vector<Grid>& grids,
              const std::vector<std::vector<int8_t>>& values, size_t index, const FixedArm& arm,
              const FixedArm& ifce);

    // The mu and scale indices (clamped) for value (y, x), from the grid's
    // values decoded so far (values not yet coded must be 0).
    void indices(const int8_t* grid, int y, int x, int& m, int& s);

private:
    const FixedArm& arm_;
    int height_, width_;
    int ifceOutputs_;
    std::vector<std::pair<int, int>> offsets_;
    std::vector<int64_t> ifce_;         // X.8, [y][x][feature] at ifceWidth_
    int ifceWidth_ = 0;
    std::vector<int64_t> context_, out_, scratch_;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
