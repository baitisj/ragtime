// Making a Cool-chic picture (docs/PICTURE_CODEC.md): the encoder fits a
// small network and its latent grids to the one picture, on the CPU's
// threads, as Cool-chic 5.0.1's PresetIntra does, then quantises the
// network and writes the stream. Fitting takes minutes (about three for
// 640x480 on four cores), so it reports how far it has got and can be
// cancelled. The code behind it is ported from Cool-chic 5.0.1 (BSD-3,
// image/src/coolchic/LICENSE.txt) and SOAP (MIT, LICENSE-SOAP.txt there).
//
// Cool-chic takes a lambda, the rate's weight, not a size. Given maxBytes,
// the encoder picks a lambda for it from the picture's size and steers it
// while it fits, from the rate the fit has reached, so the stream comes in
// just under the bytes the airtime allows.
#ifndef RAGTIME_IMAGE_COOLCHIC_ENCODE_H
#define RAGTIME_IMAGE_COOLCHIC_ENCODE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ragtime/picture/Picture.h"

namespace ragtime
{
namespace image
{

struct CoolChicOptions
{
    std::string preset = "lop";     // lop, mop, hop or vhop: bigger decodes slower
    double lambda = 0.03;           // the rate's weight: higher is smaller and blurrier
    // When over 0, the stream's most bytes: lambda is then only where the
    // fit starts if it can't guess better, and is steered to fit them.
    uint64_t maxBytes = 0;
    int iterations = 2000;          // Cool-chic's n_itr; fewer is faster and worse
    int threads = 0;                // 0: one per core
    // Progressive fitting: on this share of the steps the finest grids
    // (1 to progressiveDropped of them) are left out, so the picture from
    // the first grids of a stream cut short is a fair one too. 0: off.
    double progressiveShare = 0.5;
    int progressiveDropped = 3;
    uint64_t seed = 1;
    // How often the preview is given the picture as it stands.
    double previewSeconds = 1.0;
};

struct CoolChicResult
{
    std::vector<uint8_t> stream;
    double psnr = 0.0;          // dB, of the decoded picture against the one given
    double seconds = 0.0;       // the fit, quantisation and writing
    double lambda = 0.0;        // the rate's weight it ended with
};

// How far the encoder has got, 0 to 1; false cancels.
using CoolChicProgress = std::function<bool(double done)>;

// The picture as the fit has it so far (RGB, the image's size), as a
// decoder would show it from the stream if it were written now: now and
// then while fitting, on the encoder's thread.
using CoolChicPreview = std::function<void(const picture::Image& picture)>;

// Fits and writes image (colour, or grey taken as colour): false, with the
// reason, if it can't (a size the decoder won't take, a preset it doesn't
// know, more than maxBytes), or was cancelled (error "cancelled").
bool coolchicEncode(const picture::Image& image, const CoolChicOptions& options,
                    const CoolChicProgress& progress, CoolChicResult& out, std::string& error,
                    const CoolChicPreview& preview = nullptr);

} // namespace image
} // namespace ragtime

#endif
