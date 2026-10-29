// Cool-chic pictures (kind 0x02 on FileOffer and FileAnnounce, beside JPEG
// 2000's 0x01): Orange's overfitted codec, where each picture brings its
// own small network. This header is what the decoder and the encoder share:
// the network's shape, its latent grids and its quantised weights.
// docs/PICTURE_CODEC.md is the bitstream. The code behind it is ported from
// Cool-chic 5.0.1 (BSD-3, image/src/coolchic/LICENSE.txt).
//
// A picture is made from latent grids, the finest the picture's size and
// each next one half that, coarsest last. The ARM, a small MLP in fixed
// point, gives the range coder each latent's Laplace distribution from its
// decoded neighbours; the IFCE gives it context from the coarser grids.
// The upsampling brings every grid up to the picture's size, and the
// synthesis, a few 1x1 and 3x3 convolutions, makes the pixels from them.
// Hyperlatent grids help only the ARM, never the synthesis.
#ifndef RAGTIME_IMAGE_COOLCHIC_H
#define RAGTIME_IMAGE_COOLCHIC_H

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

// Hard caps on what a stream may ask for. Anything past them is refused
// before a byte of memory is spent on it.
constexpr int MAX_SIDE = 2048;
constexpr int64_t MAX_PIXELS = 2048 * 1536;
constexpr int MAX_GRIDS = 20;
constexpr int MAX_DOWNSAMPLING = 10;            // the coarsest grid is 1/1024
constexpr int MAX_SYNTHESIS_LAYERS = 7;
constexpr int MAX_SYNTHESIS_WIDTH = 128;
constexpr int MAX_SYNTHESIS_KERNEL = 7;
constexpr int MAX_SPATIAL_CONTEXT = 40;         // all of the 9x9 mask's causal half
constexpr int MAX_ARM_HIDDEN_LAYERS = 7;
constexpr int MAX_IFCE_OUTPUTS = 16;
constexpr int MAX_UPS_KERNEL = 16;
constexpr int MAX_PRECONCAT_KERNEL = 15;
// Latents are coded in [-LATENT_LIMIT, LATENT_LIMIT - 1].
constexpr int LATENT_LIMIT = 64;

// One synthesis layer, Cool-chic's "<out>-<kernel>-<linear|residual>-<none|relu>".
struct SynthesisLayer
{
    int outputs = 0;
    int kernel = 1;         // odd: 1 or 3 in the presets
    bool residual = false;  // adds its input (outputs must equal its inputs)
    bool relu = false;      // after the residual
};

// The network's shape: everything the header says but the byte counts.
struct Architecture
{
    int height = 0;
    int width = 0;

    // Latent grids at 1/2^latentLow to 1/2^latentHigh of the picture's size.
    int latentLow = 0;
    int latentHigh = 6;
    // Hyperlatent grids, for the ARM only; none when hyperLow > hyperHigh.
    int hyperLow = 4;
    int hyperHigh = 6;

    // The ARM: spatialContext decoded neighbours (and ifceOutputs features
    // from the IFCE) through armHiddenLayers residual layers to (mu, log scale).
    int spatialContext = 6;
    int armHiddenLayers = 2;
    bool armStabiliser = true;

    // The IFCE, for the grids at 1/2^ifceLow to 1/2^ifceHigh; none when
    // ifceOutputs is 0.
    int ifceOutputs = 2;
    int ifceLow = 0;
    int ifceHigh = 2;

    // The upsampling: transposed convolutions (even kernel) and the
    // pre-concatenation filters (odd kernel), one of each per step.
    int upsKernel = 8;
    int preconcatKernel = 7;

    // The synthesis; its last layer's outputs are the picture's channels.
    std::vector<SynthesisLayer> synthesis;
    bool synthesisStabiliser = true;

    int channels() const { return synthesis.empty() ? 0 : synthesis.back().outputs; }
    int synthesisInputs() const { return latentHigh - latentLow + 1; }
    bool hasHyperlatents() const { return hyperLow <= hyperHigh; }
};

// Cool-chic's intra presets for a picture height x width: "lop" (about
// 0.4 kB of weights), "mop", "hop" and "vhop", with its "auto" grids.
// False for any other name.
bool preset(const std::string& name, int height, int width, Architecture& out);

// Within the caps, and a shape the decoder can run. False with the reason.
bool valid(const Architecture& arch, std::string& error);

// One latent grid.
struct Grid
{
    int height = 0;
    int width = 0;
    int downsampling = 0;   // 1/2^downsampling of the picture
    bool hyper = false;     // for the ARM only
    int ifceInputs = 0;     // coarser grids the IFCE reads; 0 for none
};

// The grids in Cool-chic's order: finest first, a hyperlatent after the
// latent of its size (L0 L1 L2 L3 L4 H4 L5 H5 L6 H6 for 640x480). They
// are coded from the back, coarsest first. Empty if arch isn't valid.
std::vector<Grid> grids(const Architecture& arch);

// The four networks, in the order Cool-chic writes them.
enum Module
{
    ARM = 0,
    IFCE = 1,
    UPSAMPLING = 2,
    SYNTHESIS = 3,
    MODULES = 4
};

// The weights as sent: integers, each worth integer x 2^shift. For each
// module, all its weights in one list and all its biases in another, each
// in PyTorch's parameter order (docs/PICTURE_CODEC.md, "Weights"):
//
//   ARM          each hidden layer, the output layer, then the stabiliser;
//                a weight is [outputs][inputs], the inputs being the
//                spatial context in Cool-chic's priority order, then the
//                IFCE's features
//   IFCE         one linear layer for each grid that has one, finest grid
//                first, [ifceOutputs][ifceInputs]
//   UPSAMPLING   the latentHigh transposed convolutions' half kernels
//                ((upsKernel + 1) / 2 each, outer tap first), then the
//                pre-concatenation filters' ((preconcatKernel + 1) / 2);
//                one bias for each of them, which is sent but never used
//   SYNTHESIS    the output transform ([channels][channels]), the
//                stabiliser ([channels][synthesisInputs]), then each layer
//                ([outputs][inputs][kernel][kernel])
struct QuantisedNetwork
{
    std::vector<int32_t> weights[MODULES];
    std::vector<int32_t> biases[MODULES];
    int weightShift[MODULES] = {};  // log2 of the quantisation step, 0 or less
    int biasShift[MODULES] = {};
    int weightExpGolomb[MODULES] = {};  // the exp-Golomb order each list is coded with
    int biasExpGolomb[MODULES] = {};
};

// How many weights (or biases) a module of arch has.
size_t parameterCount(const Architecture& arch, Module module, bool biases);

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
