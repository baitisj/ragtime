// The picture codecs (docs/STAGE1.md, "Inline pictures"): JPEG 2000 both
// ways through OpenJPEG, and WebP (libwebp) and HEIF (libheif, with
// libde265 for HEVC) read from disk. JPEG, PNG and the rest are read by
// wxImage in the app. Kept out of ragtime_core, which links no codecs;
// built with -DRAGTIME_PICTURES=OFF, a stand-in says pictures can't be
// made or shown.
//
// What comes from the air is only ever decoded as a JPEG 2000 codestream,
// at most picture::MAX_DECODE_SIDE either way: image decoders are where
// an attack would come in, and that is the one format a picture is sent
// in.
#ifndef RAGTIME_IMAGE_CODEC_H
#define RAGTIME_IMAGE_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/picture/Picture.h"

namespace ragtime
{
namespace image
{

// Built with the codecs: false in the stand-in.
bool available();
// HEIF can be read (libheif, with an HEVC decoder it can find).
bool heifAvailable();
// "OpenJPEG 2.5.0, libwebp 1.3.2, libheif 1.17.6", for About and reports.
std::string versions();

// What load makes of a file's bytes.
enum class Load
{
    Done,
    NotMine,    // not JPEG 2000, WebP or HEIF: for wxImage to try
    Failed,     // one of those, but it can't be read (error says why)
};

// Reads a JPEG 2000 codestream or JP2 file, a WebP (its first frame) or a
// HEIF picture, turned the way up it says and with any transparency laid
// over white, as RGB or grey.
Load load(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error);

// How a picture was coded.
struct Coded
{
    std::vector<uint8_t> bytes;     // a JPEG 2000 codestream (.j2k)
    int layers = 0;                 // quality layers, the first a thumbnail's worth
    int resolutions = 0;
};

// Codes an image as a progressive JPEG 2000 codestream of at most
// maxBytes: the 9/7 wavelet, quality layers each about twice the one
// before in layer-resolution-component-position order, so any first part
// of it decodes to the whole picture, softer. Colour goes through the
// codestream's own colour transform, grey as one component. False, with
// the reason, if it can't be made that small.
bool encode(const picture::Image& image, uint64_t maxBytes, Coded& out, std::string& error);

// Decodes a JPEG 2000 codestream (never a JP2 file: what comes from the
// air is a codestream), or as much of one as has come: a first part
// decodes to the whole picture, softer, and the bytes past the end are
// taken as not yet here. False, with the reason, if there is no picture
// in it yet or it is bigger than maxSide either way.
bool decode(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error,
            int maxSide = picture::MAX_DECODE_SIDE);

} // namespace image
} // namespace ragtime

#endif
