// Software Name: Cool-Chic
// SPDX-FileCopyrightText: Copyright (c) 2023-2026 Orange
// SPDX-License-Identifier: BSD 3-Clause "New"
//
// This software is distributed under the BSD-3-Clause license (LICENSE.txt
// beside this file). Ported to C++ for Ragtime from Cool-chic 5.0.1
// (github.com/Orange-OpenSource/Cool-Chic, a6fe38a): the architecture of
// coolchic/component/core/coolchic.py and its initialisation. Authors: see
// Cool-chic's CONTRIBUTORS.md.
//
// The encoder's model: one picture's latent grids and the four small
// networks (ARM, IFCE, upsampling, synthesis), held as one flat array of
// floats so the optimiser, the copies of the best model and the weight
// quantisation can treat them alike.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_MODEL_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_MODEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace image
{
namespace coolchic
{

// One synthesis layer: "<out>-<k>-<linear|residual>-<none|relu>".
struct SynLayer
{
    int out = 0;
    int k = 1;          // 1 or 3
    bool residual = false;
    bool relu = false;
};

// A decoder's architecture (Cool-chic's cfg/dec/intra/*.cfg), with the
// "auto" latent resolutions of an image under a million pixels.
struct Arch
{
    std::string name;
    int latentHi = 6;           // latent grids at 1/2^0 .. 1/2^latentHi
    bool hyper = true;          // hyperlatent grids at 1/2^hyperLo .. 1/2^hyperHi
    int hyperLo = 4;
    int hyperHi = 6;
    int armContext = 6;         // spatial context pixels
    int armHidden = 2;          // residual hidden layers
    bool armStabiliser = true;
    int ifceOut = 2;            // inter-feature context values (0: no IFCE)
    int ifceLo = 0;             // grids at 1/2^ifceLo .. 1/2^ifceHi get an IFCE
    int ifceHi = 2;
    std::vector<SynLayer> synthesis;
    bool synStabiliser = true;
    int upsK = 8;               // upsampling (transposed) kernel, even
    int preconcatK = 7;         // pre-concatenation kernel, odd

    int armInputs() const { return armContext + ifceOut; }
    int synInputs() const { return latentHi + 1; }
    int synOutputs() const { return synthesis.empty() ? 0 : synthesis.back().out; }
};

// lop, mop, hop or vhop; false for any other name.
bool archByName(const std::string& name, Arch& out);

// One latent grid, in Cool-chic's order: finest first, and at each level
// the latent grid before the hyperlatent grid of the same size.
struct Grid
{
    int level = 0;          // 1/2^level of the picture
    int h = 0;
    int w = 0;
    bool hyper = false;     // only for the entropy model, not the synthesis
    int ifceIn = 0;         // inputs of this grid's IFCE, 0 for none
    int synIndex = -1;      // index among the synthesis grids, -1 for a hyperlatent
};

// Where a tensor sits in the flat parameter array.
enum class Module
{
    Latent,
    Arm,
    Ifce,
    Upsampling,
    Synthesis,
};

struct Tensor
{
    std::string name;       // PyTorch's state-dict name, for comparisons
    Module module = Module::Latent;
    bool bias = false;
    std::vector<int> shape;
    size_t offset = 0;
    size_t size = 0;
    bool trained = true;    // false: Cool-chic never moves it (unused biases)
};

// The ARM's (or one IFCE's) linear layer.
struct LinearRef
{
    int in = 0;
    int out = 0;
    bool residual = false;
    int weight = -1;        // tensor indices
    int bias = -1;
};

// A synthesis layer's tensors.
struct ConvRef
{
    SynLayer layer;
    int in = 0;
    int weight = -1;        // [out][in][k][k]
    int bias = -1;
};

struct Model
{
    Arch arch;
    int height = 0;
    int width = 0;
    int gain = 16;          // encoder gain on the latents before quantisation
    std::vector<Grid> grids;
    std::vector<int> synGrid;   // synthesis grid i (0 finest) -> index in grids

    std::vector<Tensor> tensors;
    size_t count = 0;           // floats in the parameter array

    std::vector<int> latent;            // tensor per grid
    std::vector<LinearRef> armLayers;   // hidden layers, then the output layer
    LinearRef armStabiliser;            // weight -1 if none
    std::vector<LinearRef> ifce;        // per grid; weight -1 if none
    std::vector<int> upsWeight;         // per upsampling step: the 1D half-kernel
    std::vector<int> upsBias;
    std::vector<int> preWeight;         // pre-concatenation half-kernels
    std::vector<int> preBias;
    std::vector<ConvRef> synLayers;
    ConvRef synStabiliser;              // weight -1 if none
    ConvRef synOutput;                  // the output transform: identity, never trained

    // Context pixel offsets (dy, dx) in the ARM's priority order.
    std::vector<int> ctxDy;
    std::vector<int> ctxDx;

    size_t latentCount() const;
    int tensorIndex(const std::string& name) const;     // -1 if none
};

// Builds the model for a picture height x width (sizes, tensors, offsets);
// false if the architecture or the size is out of range.
bool buildModel(const Arch& arch, int height, int width, Model& out, std::string& error);

// Cool-chic's encoder gain for a rate constraint lambda: 16, or 20 below
// 0.0005 and 24 below 0.0002.
int gainForLambda(double lambda);

// Cool-chic's initialisation of every parameter (latents zero, residual
// layers zero, the rest random as its modules do), from seed.
void initParams(const Model& model, uint64_t seed, std::vector<float>& params);

// A small, fast random source: xoshiro256**, with normal deviates.
class Random
{
public:
    explicit Random(uint64_t seed = 1);
    uint64_t next();
    double uniform();           // [0, 1)
    float normal();             // standard normal
    void normals(float* out, size_t count);

private:
    uint64_t s_[4];
    bool haveSpare_ = false;
    float spare_ = 0.0f;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
