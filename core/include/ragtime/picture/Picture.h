// Inline pictures (docs/STAGE1.md, "Inline pictures"): a picture picked
// from disk is shrunk to fit a size, coded as progressive JPEG 2000 to fit
// an airtime, and sent as a file the far end shows among the messages as
// it comes. This is the part with no codec in it: the image in memory, the
// detail-preserving downscaler, turning the camera's way up, reading a
// codestream's size, and what fits a number of seconds on the air. The
// codecs themselves (OpenJPEG, libwebp, libheif) are in image/.
#ifndef RAGTIME_PICTURE_PICTURE_H
#define RAGTIME_PICTURE_PICTURE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/link/Data2GLink.h"

namespace ragtime
{
namespace picture
{

// An image in memory: 8-bit sRGB, rows top to bottom, each pixel its
// channels side by side (3 for colour, 1 for grey).
struct Image
{
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<uint8_t> pixels;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    // Sized and filled with value.
    static Image blank(int width, int height, int channels, uint8_t value = 0);
    uint8_t* row(int y) { return pixels.data() + (size_t)y * (size_t)width * (size_t)channels; }
    const uint8_t* row(int y) const { return pixels.data() + (size_t)y * (size_t)width * (size_t)channels; }
};

// The size an image width × height takes to fit maxSide on its longer
// side, keeping its shape; never bigger than it is, and at least 1 × 1.
void fitWithin(int width, int height, int maxSide, int& outWidth, int& outHeight);

// How strongly downscale weights the pixels that stand out: 0 is a plain
// area average; the paper shows 0.5 and 1.0. 0.5 keeps thin lines and text
// without making noise and JPEG blocks shout.
constexpr double DETAIL_LAMBDA = 0.5;

// Shrinks an image to outWidth × outHeight with "Rapid, Detail-Preserving
// Image Downscaling" (Weber, Waechter, Amirshahi and Goesele, ACM
// Transactions on Graphics 35(6), 2016). Each output pixel is a weighted
// average of the input pixels under it, and a pixel weighs more the more
// it differs from a smoothed guide of the output there, so the details an
// area average washes out (a wire, an eye, the strokes of a callsign)
// survive. Averages are taken in linear light, as light adds; differences
// in sRGB, as eyes see them. An image no bigger than asked comes back as
// it is.
Image downscale(const Image& in, int outWidth, int outHeight, double lambda = DETAIL_LAMBDA);

// Grey (every pixel's channels within a step or two of each other): coded
// as one channel, it costs less.
bool isGrey(const Image& in);
Image toGrey(const Image& in);

// Puts an image the way up its EXIF orientation (1 to 8) says. Anything
// else leaves it as it is.
Image orient(const Image& in, int orientation);

// The EXIF orientation in a JPEG file's bytes (its APP1 Exif segment,
// IFD0 tag 0x0112): 1 to 8, or 1 when there is none. Phones keep the
// pixels the way the sensor saw them and say which way is up here.
int jpegOrientation(const uint8_t* bytes, size_t length);

// A JPEG 2000 codestream's size, from its SIZ marker segment: false if
// the bytes don't start with SOC and SIZ.
bool codestreamSize(const uint8_t* bytes, size_t length, int& width, int& height, int& components);

// What a JPEG 2000 codestream starts with (SOC, then SIZ), and a JP2
// file's signature box.
bool isCodestream(const uint8_t* bytes, size_t length);
bool isJp2File(const uint8_t* bytes, size_t length);

//-------------------------------------------------------------------------
// What fits the air
//-------------------------------------------------------------------------

// Pictures go as files, so they keep a file's limits.
constexpr uint64_t MAX_PICTURE_BYTES = 300 * 1024;
// Less than this isn't a picture: OpenJPEG's headers and the smallest
// thumbnail's first layer take about this much.
constexpr uint64_t MIN_PICTURE_BYTES = 300;
// The sender's choices, and their defaults (Jeff, 2026-10-27: at most
// 640 pixels either way).
constexpr int DEFAULT_MAX_SIDE = 640;
constexpr int MIN_SIDE = 32;
constexpr int MAX_SIDE = 2048;
constexpr int DEFAULT_SECONDS = 60;
constexpr int MIN_SECONDS = 5;
constexpr int MAX_SECONDS = 30 * 60;
// A picture received without asking may be this big at most; bigger ones
// wait for Receive, as other files do.
constexpr uint64_t AUTO_PICTURE_BYTES = 64 * 1024;
// A session picture is planned for the gear that suits the far station's
// path over this long (Stations::pathFrom; Jeff, 2026-10-27).
constexpr uint64_t PATH_WINDOW_MS = 5 * 60 * 1000;
// One taken from the air is decoded only up to this size either way.
constexpr int MAX_DECODE_SIDE = 4096;

// The most bytes of a broadcast picture called name whose first pass
// (its announces and pieces, as BroadcastFiles sends them) fits seconds
// of airtime in mode, from callsign; 0 if not even MIN_PICTURE_BYTES do.
uint64_t broadcastBytesFor(double seconds, const std::string& name, const std::string& callsign,
                           const link::ModeInfo& mode);

// The airtime of the seal that follows a broadcast picture, from
// callsign in mode: one burst listing it (and whatever else of ours is
// under the same seal, which this leaves out). Never cheap at the slow
// rungs, so the dialog shows it on a line of its own.
double sealSeconds(const std::string& callsign, const link::ModeInfo& mode);

// A session moves about this many bytes a second: measured, when
// measuredBytesPerSecond is over 0 (FileTraffic's acknowledged rate),
// else the mode's longest burst over its airtime, the far end's ACK
// burst and two turns of a second each (Data2G docs/arq.md, section 6).
// 0 when neither is known.
double sessionBytesPerSecond(const link::ModeInfo& mode, double measuredBytesPerSecond);

// The most bytes of a picture to send in a session in seconds at that
// rate, less the offer's own bytes; 0 if not even MIN_PICTURE_BYTES fit
// or the rate isn't known.
uint64_t sessionBytesFor(double seconds, double bytesPerSecond);

// How a picture arrives, for the dialog's preview to play: after each
// step, bytes of it have come, seconds after the first burst began. A
// step at a burst's end has its number (from 1), what the far end then
// holds; between them, steps of a few percent (at least MIN_STEP_BYTES,
// about PREVIEW_STEPS in all) show the picture sharpening, which the far
// end sees at the burst's end. The last step is the whole picture. At
// most MAX_STEPS, bursts taken together when there are more.
struct ArrivalStep
{
    uint64_t bytes = 0;
    double seconds = 0.0;
    int burst = 0;
};
constexpr size_t MAX_STEPS = 48;
constexpr size_t PREVIEW_STEPS = 32;
constexpr uint64_t MIN_STEP_BYTES = 16;
// On D2G: its first pass as BroadcastFiles sends it (estimateFile's
// bursts, and its time with the gaps between them).
std::vector<ArrivalStep> broadcastArrival(uint64_t size, const std::string& name, const std::string& callsign,
                                          const link::ModeInfo& mode);
// In a session: the mode's longest burst of file bytes at a time, at
// sessionBytesPerSecond.
std::vector<ArrivalStep> sessionArrival(uint64_t size, const link::ModeInfo& mode);

// How much of a picture has come, for under the dialog's preview (Jeff,
// 2026-10-29: "1k/40k"): "120 / 293 bytes" while it is under 1 KB, else
// in KB of 1024, one decimal under 10, "0.4k / 3.9k", "12k / 40k".
std::string comeText(uint64_t done, uint64_t whole);

// The scale under the preview's progress bar: tick positions in bytes
// from 0 up to whole, at most maxTicks + 1 of them, a round step apart
// (1, 2 or 5 times a power of ten, in bytes under 1 KB, else in KB of
// 1024); and a tick's label, "0", "500", "1.5k", "20k".
std::vector<uint64_t> scaleTicks(uint64_t whole, int maxTicks);
std::string scaleLabel(uint64_t bytes, uint64_t whole);
// The time axis over the bar (Jeff, 2026-10-29): ticks from 0 up to
// seconds, at most maxTicks + 1 of them, a round step apart (1, 2, 5, 10,
// 15, 20 or 30 s, in whole minutes from 3 minutes up); and a tick's label,
// "0", "30 s", "2 min".
std::vector<double> timeTicks(double seconds, int maxTicks);
std::string timeLabel(double tick, double seconds);

// A file name for the picture made from the one picked: its stem, made
// plain (letters, digits, '-' and '_'), cut to 40 bytes, and ".j2k".
std::string pictureName(const std::string& picked);

} // namespace picture
} // namespace ragtime

#endif
