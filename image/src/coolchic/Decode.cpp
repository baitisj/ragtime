// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD-3-Clause
//
// Ported to C++ for Ragtime from Cool-chic 5.0.1's
// coolchic/bitstream/component/coolchic.py (encode_decode_coolchic),
// latent.py (entropy_coding_latent_arm) and coolchic/bitstream/decode.py,
// with the latent part and the partial streams of docs/PICTURE_CODEC.md.
// The licence is LICENSE.txt beside this file.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Arm.h"
#include "Latents.h"
#include "RangeCoder.h"
#include "Stream.h"
#include "Upsampling.h"
#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

// One grid's decoding, kept between feeds.
struct GridState
{
    Grid grid;
    size_t count = 0;                   // values in the grid
    std::vector<int32_t> order;         // the coding order: index into its values
    size_t decoded = 0;                 // values decoded, in that order
    std::unique_ptr<GridModel> model;   // made once every coarser grid is in
    RangeDecoder coder;
    size_t runStart = 0;                // in the latent part
    uint64_t runLength = 0;
    bool complete() const { return decoded == count; }
};

} // namespace

struct Decoder::Impl
{
    State state = State::Waiting;
    std::string error;
    std::vector<uint8_t> data;

    bool haveHeader = false;
    StreamHeader header;
    bool haveWeights = false;
    QuantisedNetwork net;
    FixedArm arm;
    std::vector<FixedArm> ifce;
    UpsamplingWeights ups;
    SynthesisWeights syn;

    bool haveTable = false;
    size_t runsStart = 0;               // in the latent part, after the table
    std::vector<Grid> layout;           // grids(): finest first
    std::vector<GridState> grids;       // the same order
    std::vector<std::vector<int8_t>> values;    // each grid's latents, row by row

    State fail(const std::string& why)
    {
        error = why;
        state = State::Failed;
        return state;
    }

    State feed(const uint8_t* bytes, size_t length);
    bool readTable();
    void decodeGrids();
    void decodeGrid(size_t g, size_t arrived, bool whole);
};

Decoder::State Decoder::Impl::feed(const uint8_t* bytes, size_t length)
{
    if (state == State::Failed || state == State::Complete)
        return state;
    const size_t cap = haveHeader ? header.latentStart + header.latentBytes : MAX_STREAM_BYTES;
    length = std::min(length, cap);
    if (length > data.size())
    {
        data.reserve(cap);
        data.insert(data.end(), bytes + data.size(), bytes + length);
    }

    if (!haveHeader)
    {
        std::string why;
        const Read r = readHeaders(data.data(), data.size(), header, why);
        if (r == Read::Bad)
            return fail(why);
        if (r == Read::NeedMore)
            return state;
        haveHeader = true;
        const size_t end = header.latentStart + header.latentBytes;
        if (data.size() > end)
            data.resize(end);
        layout = coolchic::grids(header.arch);
        for (const Grid& g : layout)
        {
            GridState s;
            s.grid = g;
            s.count = size_t(g.height) * size_t(g.width);
            grids.push_back(std::move(s));
            values.emplace_back(grids.back().count, int8_t(0));
        }
    }
    if (!haveWeights)
    {
        if (data.size() < header.latentStart)
            return state;
        std::string why;
        if (!readWeights(data.data() + header.weightsStart, header, net, why) ||
            !fixedArm(header.arch, net, arm, why) || !fixedIfce(header.arch, net, ifce, why) ||
            !upsamplingWeights(header.arch, net, ups, why) ||
            !synthesisWeights(header.arch, net, syn, why))
            return fail(why);
        haveWeights = true;
        state = State::Partial;
    }
    if (!haveTable && !readTable())
        return state;
    decodeGrids();
    bool all = true;
    for (const GridState& g : grids)
        all = all && g.complete();
    if (all)
        state = State::Complete;
    return state;
}

// The grid table: each run's length, coarsest grid first, as LEB128.
bool Decoder::Impl::readTable()
{
    const uint8_t* latent = data.data() + header.latentStart;
    const size_t have = data.size() - header.latentStart;
    size_t pos = 0;
    uint64_t start = 0;
    for (size_t k = 0; k < grids.size(); k++)
    {
        uint64_t n = 0;
        int shift = 0;
        while (true)
        {
            if (pos >= have)
                return false;               // not all here yet
            if (shift > 28)
                return fail("bad grid table"), false;
            const uint8_t b = latent[pos++];
            n |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        GridState& g = grids[grids.size() - 1 - k];
        g.runStart = size_t(start);         // made absolute below
        g.runLength = n;
        start += n;
    }
    runsStart = pos;
    for (GridState& g : grids)
        g.runStart += runsStart;
    haveTable = true;
    return true;
}

void Decoder::Impl::decodeGrid(size_t i, size_t arrived, bool whole)
{
    GridState& s = grids[i];
    const uint8_t* run = data.data() + header.latentStart + s.runStart;
    if (!s.coder.started())
    {
        // The decoder reads 8 bytes at the start; until they're here (or
        // the run is whole) nothing decodes.
        if (!whole && arrived < 8)
            return;
        s.coder.start(run, arrived);
    }
    s.coder.extend(run, arrived);

    const int w = s.grid.width;
    int8_t* grid = values[i].data();
    while (s.decoded < s.count)
    {
        const int index = s.order[s.decoded];
        int m = 0, sc = 0;
        s.model->indices(grid, index / w, index % w, m, sc);
        const RangeDecoder before = s.coder;
        const int v = s.coder.decode(m, sc);
        // Good only if it came from bytes that have arrived.
        if (!whole && s.coder.pos() > arrived)
        {
            s.coder = before;
            return;
        }
        grid[index] = int8_t(v);
        s.decoded++;
    }
}

void Decoder::Impl::decodeGrids()
{
    const size_t have = data.size() - header.latentStart;
    const size_t latentEnd = header.latentBytes;
    for (size_t k = 0; k < grids.size(); k++)
    {
        const size_t i = grids.size() - 1 - k;
        GridState& s = grids[i];
        if (!s.model)
        {
            // Every coarser grid is in: the IFCE can read them.
            s.order = codingOrder(s.grid.height, s.grid.width);
            s.model.reset(new GridModel(header.arch, layout, values, i, arm, ifce[i]));
        }
        if (s.complete())
            continue;
        // What of this run has come, and whether all of it has (a run the
        // stream can't hold never will).
        const uint64_t start = s.runStart, end = start + s.runLength;
        const uint64_t present = std::min<uint64_t>(end, latentEnd);
        const uint64_t got = have > start ? std::min<uint64_t>(present, have) - start : 0;
        const bool whole = have >= end && end <= latentEnd;
        if (start > have)
            return;
        decodeGrid(i, size_t(got), whole);
        if (!s.complete())
            return;                         // finer grids wait for this one
    }
}

Decoder::Decoder() : impl_(new Impl) {}
Decoder::~Decoder() = default;

Decoder::State Decoder::feed(const uint8_t* bytes, size_t length)
{
    return impl_->feed(bytes, length);
}

Decoder::State Decoder::state() const { return impl_->state; }
const std::string& Decoder::error() const { return impl_->error; }

const Architecture* Decoder::architecture() const
{
    return impl_->haveHeader ? &impl_->header.arch : nullptr;
}

size_t Decoder::latentStart() const { return impl_->haveHeader ? impl_->header.latentStart : 0; }

size_t Decoder::streamLength() const
{
    return impl_->haveHeader ? impl_->header.latentStart + impl_->header.latentBytes : 0;
}

size_t Decoder::gridCount() const { return impl_->grids.size(); }

const std::vector<int8_t>& Decoder::latents(size_t grid) const
{
    return impl_->values[grid];
}

size_t Decoder::decoded(size_t grid) const { return impl_->grids[grid].decoded; }

bool Decoder::picture(picture::Image& out) const
{
    const Impl& d = *impl_;
    if (!d.haveWeights || d.state == State::Failed)
        return false;
    // The synthesis' grids, finest first, as they stand: the hyperlatents
    // are for the ARM only.
    std::vector<Plane> latent;
    for (size_t g = 0; g < d.grids.size(); g++)
    {
        const GridState& s = d.grids[g];
        if (s.grid.hyper)
            continue;
        Plane p = Plane::zeros(s.grid.height, s.grid.width);
        for (size_t n = 0; n < s.count; n++)
            p.v[n] = float(d.values[g][n]);
        latent.push_back(std::move(p));
    }
    const std::vector<Plane> rgb = synthesise(d.syn, upsample(d.ups, latent));
    const Architecture& a = d.header.arch;
    if (rgb.size() != 3)
        return false;
    out = picture::Image::blank(a.width, a.height, 3);
    for (int y = 0; y < a.height; y++)
    {
        uint8_t* row = out.row(y);
        for (int x = 0; x < a.width; x++)
            for (int c = 0; c < 3; c++)
            {
                // decode.py rounds to 8 bits, clamps, rounds again; png.py
                // clips and rounds half to even.
                float v = std::nearbyint(255.0f * rgb[size_t(c)].at(y, x)) / 255.0f;
                v = std::min(std::max(v, 0.0f), 1.0f);
                row[x * 3 + c] = uint8_t(std::nearbyint(v * 255.0f));
            }
    }
    return true;
}

} // namespace coolchic

bool coolchicDecode(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error)
{
    coolchic::Decoder d;
    const coolchic::Decoder::State s = d.feed(bytes, length);
    if (s == coolchic::Decoder::State::Failed)
        return error = d.error(), false;
    if (s == coolchic::Decoder::State::Waiting)
        return error = "the picture hasn't come yet", false;
    if (!d.picture(out))
        return error = "no picture", false;
    return true;
}

} // namespace image
} // namespace ragtime
