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
#include <memory>
#include <string>
#include <vector>

#include "ragtime/picture/Picture.h"

// Windows' rpcndr.h (through windows.h) defines hyper as a macro.
#ifdef hyper
#undef hyper
#endif

namespace ragtime
{
namespace image
{
namespace coolchic
{

// Hard caps on what a stream may ask for (docs/PICTURE_CODEC.md, "Limits").
// Anything past them is refused before a byte of memory is spent on it.
constexpr int MAX_SIDE = 2048;
constexpr int64_t MAX_PIXELS = 2048 * 1536;
constexpr size_t MAX_STREAM_BYTES = 300 * 1024;  // Ragtime's cap on any item
constexpr int MAX_GRIDS = 12;
constexpr int MAX_DOWNSAMPLING = 10;            // the coarsest grid is 1/1024
constexpr int MAX_SYNTHESIS_LAYERS = 7;         // the header has 3 bits for them
constexpr int MAX_SYNTHESIS_WIDTH = 64;
constexpr int MAX_SYNTHESIS_KERNEL = 7;
constexpr int MAX_SPATIAL_CONTEXT = 40;         // all of the 9x9 mask's causal half
constexpr int MAX_ARM_HIDDEN_LAYERS = 4;
constexpr int MAX_IFCE_OUTPUTS = 16;
constexpr int MAX_UPS_KERNEL = 14;
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

// Decodes a stream as it arrives (docs/PICTURE_CODEC.md, "Partial
// streams"). Each feed gives the stream's first bytes, at least as many as
// the last time; the decoder keeps its latents and its range decoder
// between feeds, so each new burst decodes only the new values. Nothing
// shows until the headers and weights are in; then the grids that have
// arrived decode exactly, the one being received as far as its bytes go,
// and the rest stay zero.
class Decoder
{
public:
    enum class State
    {
        Waiting,    // the headers and weights haven't all come
        Partial,    // a picture can be shown, but more is to come
        Complete,   // every grid is decoded
        Failed      // not a stream this decoder takes (error() says why)
    };

    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // bytes: the stream's first length bytes. Bytes past the stream's own
    // length are ignored.
    State feed(const uint8_t* bytes, size_t length);
    State state() const;
    const std::string& error() const;

    // The picture from the grids as they stand: RGB, 8 bits. False while
    // Waiting or Failed.
    bool picture(picture::Image& out) const;

    // Once the headers are in: the shape, and where the latent part starts
    // and the stream ends. 0 before.
    const Architecture* architecture() const;
    size_t latentStart() const;
    size_t streamLength() const;
    // Each grid's latents, in grids()'s order (finest first), and how many
    // of them have been decoded (in the coding order).
    size_t gridCount() const;
    const std::vector<int8_t>& latents(size_t grid) const;
    size_t decoded(size_t grid) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace coolchic

// Decodes a Cool-chic picture (kind 0x02), or as much of one as has come:
// false, with the reason, if it isn't one or not enough has come to show.
bool coolchicDecode(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error);
} // namespace image
} // namespace ragtime

#endif
