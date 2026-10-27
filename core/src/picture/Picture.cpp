#include "ragtime/picture/Picture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include "ragtime/broadcast/Airtime.h"
#include "ragtime/files/FileTraffic.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace picture
{

namespace
{

// sRGB's 8-bit steps in linear light, and back.
const std::array<float, 256>& linearTable()
{
    static const std::array<float, 256> table = []() {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; i++)
        {
            double v = i / 255.0;
            t[(size_t)i] = (float)(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

double srgbFromLinear(double v)
{
    v = std::min(1.0, std::max(0.0, v));
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

uint8_t byteFromLinear(double v)
{
    return (uint8_t)std::lround(srgbFromLinear(v) * 255.0);
}

// The input pixels under each output pixel along one axis: from first,
// each one's share of the output pixel's span (the shares sum to 1).
struct Span
{
    int first = 0;
    std::vector<double> share;
};

std::vector<Span> spans(int inSize, int outSize)
{
    std::vector<Span> out((size_t)outSize);
    double scale = (double)inSize / (double)outSize;
    for (int o = 0; o < outSize; o++)
    {
        double from = o * scale, to = (o + 1) * scale;
        int first = (int)std::floor(from);
        int last = std::min(inSize - 1, (int)std::ceil(to) - 1);
        Span& s = out[(size_t)o];
        s.first = first;
        for (int i = first; i <= last; i++)
        {
            double overlap = std::min(to, (double)(i + 1)) - std::max(from, (double)i);
            s.share.push_back(std::max(0.0, overlap) / scale);
        }
    }
    return out;
}

uint32_t read32(const uint8_t* p, bool little)
{
    return little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                  : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

uint16_t read16(const uint8_t* p, bool little)
{
    return little ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]);
}

// The orientation in a TIFF block (EXIF's body), or 1.
int tiffOrientation(const uint8_t* t, size_t length)
{
    if (length < 8) return 1;
    bool little;
    if (t[0] == 'I' && t[1] == 'I')
        little = true;
    else if (t[0] == 'M' && t[1] == 'M')
        little = false;
    else
        return 1;
    if (read16(t + 2, little) != 42) return 1;
    uint32_t ifd = read32(t + 4, little);
    if (ifd > length || length - ifd < 2) return 1;
    uint16_t count = read16(t + ifd, little);
    for (uint16_t i = 0; i < count; i++)
    {
        size_t at = (size_t)ifd + 2 + (size_t)i * 12;
        if (at + 12 > length) return 1;
        if (read16(t + at, little) != 0x0112) continue;
        // SHORT, one of them: the value sits in the first two bytes of
        // the entry's value field.
        if (read16(t + at + 2, little) != 3) return 1;
        int v = read16(t + at + 8, little);
        return v >= 1 && v <= 8 ? v : 1;
    }
    return 1;
}

} // namespace

Image Image::blank(int width, int height, int channels, uint8_t value)
{
    Image image;
    image.width = std::max(0, width);
    image.height = std::max(0, height);
    image.channels = channels;
    image.pixels.assign((size_t)image.width * (size_t)image.height * (size_t)channels, value);
    return image;
}

void fitWithin(int width, int height, int maxSide, int& outWidth, int& outHeight)
{
    outWidth = std::max(1, width);
    outHeight = std::max(1, height);
    maxSide = std::max(1, maxSide);
    int longer = std::max(outWidth, outHeight);
    if (longer <= maxSide) return;
    double scale = (double)maxSide / (double)longer;
    if (outWidth >= outHeight)
    {
        outHeight = std::max(1, (int)std::lround(outHeight * scale));
        outWidth = maxSide;
    }
    else
    {
        outWidth = std::max(1, (int)std::lround(outWidth * scale));
        outHeight = maxSide;
    }
}

Image downscale(const Image& in, int outWidth, int outHeight, double lambda)
{
    if (in.empty() || outWidth <= 0 || outHeight <= 0) return Image();
    if (outWidth >= in.width && outHeight >= in.height) return in;
    outWidth = std::min(outWidth, in.width);
    outHeight = std::min(outHeight, in.height);
    const int c = in.channels;
    const auto& lin = linearTable();
    std::vector<Span> xs = spans(in.width, outWidth), ys = spans(in.height, outHeight);

    // 1. The area average of each output pixel, in linear light.
    std::vector<double> average((size_t)outWidth * (size_t)outHeight * (size_t)c, 0.0);
    for (int oy = 0; oy < outHeight; oy++)
    {
        const Span& sy = ys[(size_t)oy];
        for (int ox = 0; ox < outWidth; ox++)
        {
            const Span& sx = xs[(size_t)ox];
            double* a = &average[((size_t)oy * (size_t)outWidth + (size_t)ox) * (size_t)c];
            for (size_t j = 0; j < sy.share.size(); j++)
            {
                const uint8_t* row = in.row(sy.first + (int)j);
                for (size_t i = 0; i < sx.share.size(); i++)
                {
                    double w = sy.share[j] * sx.share[i];
                    const uint8_t* p = row + (size_t)(sx.first + (int)i) * (size_t)c;
                    for (int k = 0; k < c; k++) a[k] += w * lin[p[k]];
                }
            }
        }
    }

    // 2. The guide: the average smoothed by the 3 × 3 binomial kernel
    // (1 2 1 by 1 2 1, over 16), edges held, then in sRGB for comparing.
    std::vector<double> guide(average.size(), 0.0);
    static const double tap[3] = {0.25, 0.5, 0.25};
    for (int oy = 0; oy < outHeight; oy++)
    {
        for (int ox = 0; ox < outWidth; ox++)
        {
            double* g = &guide[((size_t)oy * (size_t)outWidth + (size_t)ox) * (size_t)c];
            for (int dy = -1; dy <= 1; dy++)
            {
                int y = std::min(outHeight - 1, std::max(0, oy + dy));
                for (int dx = -1; dx <= 1; dx++)
                {
                    int x = std::min(outWidth - 1, std::max(0, ox + dx));
                    const double* a = &average[((size_t)y * (size_t)outWidth + (size_t)x) * (size_t)c];
                    double w = tap[dy + 1] * tap[dx + 1];
                    for (int k = 0; k < c; k++) g[k] += w * a[k];
                }
            }
            for (int k = 0; k < c; k++) g[k] = srgbFromLinear(g[k]);
        }
    }

    // 3. Each output pixel again, every input pixel under it weighted by
    // its share and by how far it stands from the guide, to the power
    // lambda; the guide's distance runs 0 to 1 (the colour cube's
    // diagonal). A flat patch, where every pixel is the guide, keeps the
    // average.
    Image out = Image::blank(outWidth, outHeight, c);
    const double vmax = std::sqrt((double)c);
    for (int oy = 0; oy < outHeight; oy++)
    {
        const Span& sy = ys[(size_t)oy];
        uint8_t* outRow = out.row(oy);
        for (int ox = 0; ox < outWidth; ox++)
        {
            const Span& sx = xs[(size_t)ox];
            size_t at = ((size_t)oy * (size_t)outWidth + (size_t)ox) * (size_t)c;
            const double* g = &guide[at];
            double sum[4] = {0, 0, 0, 0};
            double total = 0.0;
            for (size_t j = 0; j < sy.share.size(); j++)
            {
                const uint8_t* row = in.row(sy.first + (int)j);
                for (size_t i = 0; i < sx.share.size(); i++)
                {
                    const uint8_t* p = row + (size_t)(sx.first + (int)i) * (size_t)c;
                    double d2 = 0.0;
                    for (int k = 0; k < c; k++)
                    {
                        double d = p[k] / 255.0 - g[k];
                        d2 += d * d;
                    }
                    double w = sy.share[j] * sx.share[i] * (lambda == 0.0 ? 1.0 : std::pow(std::sqrt(d2) / vmax, lambda));
                    for (int k = 0; k < c; k++) sum[k] += w * lin[p[k]];
                    total += w;
                }
            }
            uint8_t* o = outRow + (size_t)ox * (size_t)c;
            for (int k = 0; k < c; k++)
                o[k] = byteFromLinear(total > 1e-12 ? sum[k] / total : average[at + (size_t)k]);
        }
    }
    return out;
}

bool isGrey(const Image& in)
{
    if (in.channels == 1) return true;
    if (in.channels < 3) return false;
    for (size_t i = 0; i + 2 < in.pixels.size(); i += (size_t)in.channels)
    {
        int r = in.pixels[i], g = in.pixels[i + 1], b = in.pixels[i + 2];
        if (std::abs(r - g) > 2 || std::abs(g - b) > 2 || std::abs(r - b) > 2) return false;
    }
    return true;
}

Image toGrey(const Image& in)
{
    if (in.channels == 1) return in;
    Image out = Image::blank(in.width, in.height, 1);
    for (size_t i = 0, o = 0; o < out.pixels.size(); i += (size_t)in.channels, o++)
        out.pixels[o] = (uint8_t)((in.pixels[i] + in.pixels[i + 1] + in.pixels[i + 2] + 1) / 3);
    return out;
}

Image orient(const Image& in, int orientation)
{
    if (in.empty() || orientation < 2 || orientation > 8) return in;
    bool swap = orientation >= 5;
    int w = in.width, h = in.height;
    Image out = Image::blank(swap ? h : w, swap ? w : h, in.channels);
    const size_t c = (size_t)in.channels;
    for (int y = 0; y < out.height; y++)
    {
        for (int x = 0; x < out.width; x++)
        {
            int ix = x, iy = y;
            switch (orientation)
            {
                case 2: ix = w - 1 - x; break;                       // mirrored
                case 3: ix = w - 1 - x; iy = h - 1 - y; break;       // upside down
                case 4: iy = h - 1 - y; break;                       // mirrored upside down
                case 5: ix = y; iy = x; break;                       // transposed
                case 6: ix = y; iy = h - 1 - x; break;               // turned a quarter right
                case 7: ix = w - 1 - y; iy = h - 1 - x; break;       // transversed
                case 8: ix = w - 1 - y; iy = x; break;               // turned a quarter left
                default: break;
            }
            std::memcpy(out.row(y) + (size_t)x * c, in.row(iy) + (size_t)ix * c, c);
        }
    }
    return out;
}

int jpegOrientation(const uint8_t* bytes, size_t length)
{
    if (length < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return 1;
    size_t at = 2;
    while (at + 4 <= length)
    {
        if (bytes[at] != 0xFF) return 1;
        uint8_t marker = bytes[at + 1];
        if (marker == 0xFF)
        {
            at++;
            continue;
        }
        // Start of scan or end of image: no Exif before the pixels.
        if (marker == 0xDA || marker == 0xD9) return 1;
        size_t segment = (size_t)bytes[at + 2] << 8 | bytes[at + 3];
        if (segment < 2 || at + 2 + segment > length) return 1;
        const uint8_t* body = bytes + at + 4;
        size_t bodyLength = segment - 2;
        if (marker == 0xE1 && bodyLength >= 6 && std::memcmp(body, "Exif\0\0", 6) == 0)
            return tiffOrientation(body + 6, bodyLength - 6);
        at += 2 + segment;
    }
    return 1;
}

bool isCodestream(const uint8_t* bytes, size_t length)
{
    return length >= 4 && bytes[0] == 0xFF && bytes[1] == 0x4F && bytes[2] == 0xFF && bytes[3] == 0x51;
}

bool isJp2File(const uint8_t* bytes, size_t length)
{
    static const uint8_t signature[12] = {0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
    return length >= 12 && std::memcmp(bytes, signature, 12) == 0;
}

bool codestreamSize(const uint8_t* bytes, size_t length, int& width, int& height, int& components)
{
    // SOC, SIZ, Lsiz, Rsiz, then Xsiz, Ysiz, XOsiz, YOsiz (4 each),
    // the tile's four, and Csiz (2).
    if (!isCodestream(bytes, length) || length < 4 + 2 + 2 + 32 + 2) return false;
    const uint8_t* p = bytes + 8;
    uint32_t xsiz = read32(p, false), ysiz = read32(p + 4, false);
    uint32_t xo = read32(p + 8, false), yo = read32(p + 12, false);
    if (xsiz <= xo || ysiz <= yo || xsiz - xo > 1u << 20 || ysiz - yo > 1u << 20) return false;
    width = (int)(xsiz - xo);
    height = (int)(ysiz - yo);
    components = read16(p + 32, false);
    return components > 0;
}

uint64_t broadcastBytesFor(double seconds, const std::string& name, const std::string& callsign,
                           const link::ModeInfo& mode)
{
    auto fits = [&](uint64_t size) {
        return broadcast::estimateFile(size, name, callsign, mode).airSeconds <= seconds;
    };
    if (!(seconds > 0) || !fits(MIN_PICTURE_BYTES)) return 0;
    if (fits(MAX_PICTURE_BYTES)) return MAX_PICTURE_BYTES;
    uint64_t lo = MIN_PICTURE_BYTES, hi = MAX_PICTURE_BYTES;
    while (hi - lo > 1)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

double sealSeconds(const std::string& callsign, const link::ModeInfo& mode)
{
    protocol::Record record;
    record.header.callsign = protocol::normalCallsign(callsign);
    if (record.header.callsign.empty()) record.header.callsign = "N0CALL";
    protocol::Seal seal;
    seal.number = 1;
    seal.items.resize(1);
    record.body = seal;
    std::vector<std::vector<uint8_t>> frames = broadcast::packFrames({record});
    return frames.empty() ? 0.0 : broadcast::burstSeconds(frames, mode);
}

double sessionBytesPerSecond(const link::ModeInfo& mode, double measuredBytesPerSecond)
{
    if (measuredBytesPerSecond > 0) return measuredBytesPerSecond;
    uint64_t burst = files::FileTraffic::burstBytes(mode);
    if (burst == 0) return 0.0;
    int codewords = (int)(burst / (uint64_t)mode.bytesPerCodeword);
    double each = mode.maxCodewords > 1 ? (mode.secondsAtMax - mode.secondsAtOne) / (mode.maxCodewords - 1) : 0.0;
    double data = mode.secondsAtOne + (codewords - 1) * std::max(0.0, each);
    // The far end's reply carries the ACK: a burst of control codewords,
    // no longer than a burst of one; each turn takes about a second.
    double cycle = data + mode.secondsAtOne + 2.0;
    return cycle > 0 ? (double)burst / cycle : 0.0;
}

uint64_t sessionBytesFor(double seconds, double bytesPerSecond)
{
    if (!(seconds > 0) || !(bytesPerSecond > 0)) return 0;
    // The offer is about 40 bytes, and each FileData carries 18 of its own
    // ahead of at least 256 of the file's (FileTraffic::MIN_PIECE).
    double payload = seconds * bytesPerSecond - 40.0;
    double bytes = payload * 256.0 / (256.0 + 18.0);
    if (bytes < (double)MIN_PICTURE_BYTES) return 0;
    return std::min<uint64_t>(MAX_PICTURE_BYTES, (uint64_t)bytes);
}

namespace
{

// Steps evenly through n bursts of a picture of size bytes taking seconds
// in all, at most MAX_STEPS of them.
std::vector<ArrivalStep> evenSteps(uint64_t size, uint64_t bursts, double seconds)
{
    std::vector<ArrivalStep> steps;
    if (size == 0 || bursts == 0) return steps;
    uint64_t count = std::min<uint64_t>(bursts, MAX_STEPS);
    for (uint64_t i = 1; i <= count; i++)
    {
        // The burst this step ends at, so the last is the whole.
        uint64_t burst = (bursts * i + count - 1) / count;
        ArrivalStep step;
        step.bytes = i == count ? size : std::min(size, size * burst / bursts);
        step.seconds = seconds * (double)burst / (double)bursts;
        steps.push_back(step);
    }
    return steps;
}

} // namespace

std::vector<ArrivalStep> broadcastArrival(uint64_t size, const std::string& name, const std::string& callsign,
                                          const link::ModeInfo& mode)
{
    if (!mode.valid() || size == 0) return {};
    broadcast::FileEstimate estimate = broadcast::estimateFile(size, name, callsign, mode);
    double seconds = estimate.wallSeconds > 0 ? estimate.wallSeconds : estimate.airSeconds;
    return evenSteps(size, (uint64_t)std::max(1, estimate.bursts), seconds);
}

std::vector<ArrivalStep> sessionArrival(uint64_t size, const link::ModeInfo& mode)
{
    uint64_t burst = files::FileTraffic::burstBytes(mode);
    double rate = sessionBytesPerSecond(mode, 0.0);
    if (size == 0 || burst == 0 || !(rate > 0)) return {};
    return evenSteps(size, (size + burst - 1) / burst, (double)size / rate);
}

std::string pictureName(const std::string& picked)
{
    std::string base = picked;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    std::string plain;
    for (char ch : base)
    {
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-'
                    || ch == '_';
        if (keep)
            plain += ch;
        else if (!plain.empty() && plain.back() != '-')
            plain += '-';
        if (plain.size() >= 40) break;
    }
    while (!plain.empty() && plain.back() == '-') plain.pop_back();
    if (plain.empty()) plain = "picture";
    return plain + ".j2k";
}

} // namespace picture
} // namespace ragtime
