// Inline pictures: the detail-preserving downscaler, orientation, what fits
// the air (core), and progressive JPEG 2000 both ways and WebP from disk
// (the image library).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/image/Codec.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/picture/Picture.h"

#if RAGTIME_TEST_WEBP
#include <webp/encode.h>
#endif

using namespace ragtime;
using picture::Image;

namespace
{

// A test card: colour ramps, a few hard edges and a little noise, as a
// photograph has.
Image testCard(int width, int height)
{
    Image image = Image::blank(width, height, 3);
    std::mt19937 noise(7);
    std::uniform_int_distribution<int> grain(-6, 6);
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            uint8_t* p = image.row(y) + x * 3;
            int r = x * 255 / std::max(1, width - 1), g = y * 255 / std::max(1, height - 1);
            int b = ((x / 40 + y / 40) % 2) * 160 + 40;
            if (std::abs(x - width / 2) < 3) r = g = b = 20;    // a dark post
            int n = grain(noise);
            p[0] = (uint8_t)std::clamp(r + n, 0, 255);
            p[1] = (uint8_t)std::clamp(g + n, 0, 255);
            p[2] = (uint8_t)std::clamp(b + n, 0, 255);
        }
    }
    return image;
}

double psnr(const Image& a, const Image& b)
{
    if (a.width != b.width || a.height != b.height || a.channels != b.channels) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.pixels.size(); i++)
    {
        double d = (double)a.pixels[i] - (double)b.pixels[i];
        sum += d * d;
    }
    double mse = sum / (double)a.pixels.size();
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void testFit()
{
    int w = 0, h = 0;
    picture::fitWithin(4000, 3000, 640, w, h);
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 480);
    picture::fitWithin(1000, 3000, 640, w, h);
    CHECK_EQ(w, 213);
    CHECK_EQ(h, 640);
    picture::fitWithin(300, 200, 640, w, h);    // never made bigger
    CHECK_EQ(w, 300);
    CHECK_EQ(h, 200);
    picture::fitWithin(5000, 2, 640, w, h);
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 1);
}

void testDownscale()
{
    // A flat field stays flat.
    Image grey = Image::blank(64, 48, 3, 90);
    Image small = picture::downscale(grey, 16, 12);
    CHECK_EQ(small.width, 16);
    CHECK_EQ(small.height, 12);
    bool flat = std::all_of(small.pixels.begin(), small.pixels.end(), [](uint8_t v) { return v == 90; });
    CHECK(flat);

    // Light adds: a fine checkerboard of black and white averages to half
    // the light, which sRGB writes as 188, not 128.
    Image check = Image::blank(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) check.row(y)[x] = (x + y) % 2 ? 255 : 0;
    Image half = picture::downscale(check, 16, 16, 0.0);
    CHECK(std::abs((int)half.row(8)[8] - 188) <= 2);

    // A one-pixel wire across white, shrunk eight times: an area average
    // turns it into a faint grey; the detail-preserving one keeps it dark.
    Image wire = Image::blank(256, 256, 3, 255);
    for (int x = 0; x < 256; x++)
    {
        uint8_t* p = wire.row(100) + x * 3;
        p[0] = p[1] = p[2] = 0;
    }
    Image boxed = picture::downscale(wire, 32, 32, 0.0);
    Image kept = picture::downscale(wire, 32, 32);
    int boxedWire = boxed.row(12)[16 * 3], keptWire = kept.row(12)[16 * 3];
    CHECK(boxedWire > 200);         // all but washed out
    CHECK(keptWire < boxedWire - 40);
    CHECK_EQ((int)kept.row(4)[4 * 3], 255);     // the white around it stays white

    // Asked no smaller than it is: the same picture back.
    Image same = picture::downscale(wire, 300, 300);
    CHECK_EQ(same.width, 256);
    CHECK(same.pixels == wire.pixels);
}

void testOrient()
{
    // 3 wide, 2 high, each pixel numbered.
    Image in = Image::blank(3, 2, 1);
    for (int i = 0; i < 6; i++) in.pixels[(size_t)i] = (uint8_t)i;
    // 6: turned a quarter right. The bottom-left pixel comes to the top left.
    Image r = picture::orient(in, 6);
    CHECK_EQ(r.width, 2);
    CHECK_EQ(r.height, 3);
    CHECK_EQ((int)r.row(0)[0], 3);
    CHECK_EQ((int)r.row(0)[1], 0);
    CHECK_EQ((int)r.row(2)[0], 5);
    // 8: a quarter left. The top-right pixel comes to the top left.
    Image l = picture::orient(in, 8);
    CHECK_EQ((int)l.row(0)[0], 2);
    CHECK_EQ((int)l.row(2)[1], 3);
    // 3: upside down.
    Image u = picture::orient(in, 3);
    CHECK_EQ((int)u.row(0)[0], 5);
    // 1, or nonsense: as it is.
    CHECK(picture::orient(in, 1).pixels == in.pixels);
    CHECK(picture::orient(in, 9).pixels == in.pixels);
}

void testJpegOrientation()
{
    // SOI, an APP0, then APP1 "Exif" with a big-endian TIFF whose IFD0
    // holds Orientation = 6, then SOS.
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00};
    std::vector<uint8_t> tiff = {'M', 'M', 0, 42, 0, 0, 0, 8,   // header, IFD0 at 8
                                 0, 1,                          // one entry
                                 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0,
                                 0, 0, 0, 0};                   // no next IFD
    std::vector<uint8_t> app1 = {'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), tiff.begin(), tiff.end());
    size_t length = app1.size() + 2;
    jpeg.insert(jpeg.end(), {0xFF, 0xE1, (uint8_t)(length >> 8), (uint8_t)length});
    jpeg.insert(jpeg.end(), app1.begin(), app1.end());
    jpeg.insert(jpeg.end(), {0xFF, 0xDA, 0x00, 0x02});
    CHECK_EQ(picture::jpegOrientation(jpeg.data(), jpeg.size()), 6);
    // Little-endian, orientation 8.
    std::vector<uint8_t> le = jpeg;
    std::vector<uint8_t> tiffLe = {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0};
    std::copy(tiffLe.begin(), tiffLe.end(), le.begin() + 8 + 4 + 6);
    CHECK_EQ(picture::jpegOrientation(le.data(), le.size()), 8);
    // Cut short, or not a JPEG: 1.
    CHECK_EQ(picture::jpegOrientation(jpeg.data(), 14), 1);
    const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    CHECK_EQ(picture::jpegOrientation(png, sizeof png), 1);
}

void testAirtime()
{
    const std::vector<link::ModeInfo>& modes = link::knownModes();
    CHECK(!modes.empty());
    // The fastest and the slowest modes known.
    link::ModeInfo fast = modes.front(), slow = modes.front();
    for (const link::ModeInfo& m : modes)
    {
        double rate = m.bytesPerCodeword * m.maxCodewords / std::max(0.1, m.secondsAtMax);
        if (rate > fast.bytesPerCodeword * fast.maxCodewords / std::max(0.1, fast.secondsAtMax)) fast = m;
        if (rate < slow.bytesPerCodeword * slow.maxCodewords / std::max(0.1, slow.secondsAtMax)) slow = m;
    }
    uint64_t oneMinute = picture::broadcastBytesFor(60, "sunset.j2k", "AG7EW", fast);
    uint64_t twoMinutes = picture::broadcastBytesFor(120, "sunset.j2k", "AG7EW", fast);
    CHECK(oneMinute >= picture::MIN_PICTURE_BYTES);
    CHECK(twoMinutes > oneMinute);
    // It fits, and a byte more than a little over it doesn't.
    CHECK(broadcast::estimateFile(oneMinute, "sunset.j2k", "AG7EW", fast).airSeconds <= 60.0);
    if (oneMinute < picture::MAX_PICTURE_BYTES)
        CHECK(broadcast::estimateFile(oneMinute + 1, "sunset.j2k", "AG7EW", fast).airSeconds > 60.0);
    // The slowest gear gives less in the same time, and 5 s may give none.
    CHECK(picture::broadcastBytesFor(60, "sunset.j2k", "AG7EW", slow) < oneMinute);
    CHECK_EQ(picture::broadcastBytesFor(0, "sunset.j2k", "AG7EW", fast), (uint64_t)0);

    // A session: measured beats the mode's figure.
    CHECK_EQ(picture::sessionBytesPerSecond(fast, 123.0), 123.0);
    double modeRate = picture::sessionBytesPerSecond(fast, 0.0);
    CHECK(modeRate > 0);
    CHECK_EQ(picture::sessionBytesPerSecond(link::ModeInfo(), 0.0), 0.0);
    uint64_t session = picture::sessionBytesFor(60, 100.0);
    CHECK(session > 5000 && session < 6000);
    CHECK_EQ(picture::sessionBytesFor(1, 100.0), (uint64_t)0);   // too little for a picture
    CHECK_EQ(picture::sessionBytesFor(60, 0.0), (uint64_t)0);
}

void testNames()
{
    CHECK_EQ(picture::pictureName("/home/jeff/Pictures/IMG_2041.HEIC"), std::string("IMG_2041.j2k"));
    CHECK_EQ(picture::pictureName("C:\\Users\\jeff\\my antenna (new).jpg"), std::string("my-antenna-new.j2k"));
    CHECK_EQ(picture::pictureName("\xc3\xa9t\xc3\xa9.png"), std::string("t.j2k"));
    CHECK_EQ(picture::pictureName(".png"), std::string("png.j2k"));
    CHECK_EQ(picture::pictureName("!!!"), std::string("picture.j2k"));
}

void testCodec()
{
    if (!image::available())
    {
        std::printf("Built without pictures: codec checks skipped\n");
        return;
    }
    Image card = testCard(640, 480);
    for (uint64_t budget : {1500ull, 8000ull, 30000ull})
    {
        image::Coded coded;
        std::string error;
        CHECK(image::encode(card, budget, coded, error));
        CHECK(coded.bytes.size() <= budget);
        CHECK(coded.bytes.size() >= budget * 80 / 100);
        CHECK(coded.layers >= 1);
        CHECK(picture::isCodestream(coded.bytes.data(), coded.bytes.size()));
        // OpenJPEG's "Created by" comment is gone: nothing but pixels on air.
        std::string text(coded.bytes.begin(), coded.bytes.end());
        CHECK(text.find("OpenJPEG") == std::string::npos);
        int w = 0, h = 0, comps = 0;
        CHECK(picture::codestreamSize(coded.bytes.data(), coded.bytes.size(), w, h, comps));
        CHECK_EQ(w, 640);
        CHECK_EQ(h, 480);
        CHECK_EQ(comps, 3);

        Image whole;
        CHECK(image::decode(coded.bytes.data(), coded.bytes.size(), whole, error));
        CHECK_EQ(whole.width, 640);
        CHECK_EQ(whole.channels, 3);
        double full = psnr(card, whole);
        CHECK(full > (budget >= 8000 ? 22.0 : 17.0));

        // Any first part is the whole picture, softer.
        Image part;
        size_t third = coded.bytes.size() / 3;
        CHECK(image::decode(coded.bytes.data(), third, part, error));
        CHECK_EQ(part.width, 640);
        CHECK_EQ(part.height, 480);
        double soft = psnr(card, part);
        CHECK(soft > 12.0);
        CHECK(soft <= full + 0.01);
    }

    // More bytes, a better picture.
    image::Coded small, big;
    std::string error;
    CHECK(image::encode(card, 2000, small, error));
    CHECK(image::encode(card, 20000, big, error));
    Image a, b;
    CHECK(image::decode(small.bytes.data(), small.bytes.size(), a, error));
    CHECK(image::decode(big.bytes.data(), big.bytes.size(), b, error));
    CHECK(psnr(card, b) > psnr(card, a) + 3.0);

    // Grey goes as one component.
    Image grey = picture::toGrey(card);
    CHECK(picture::isGrey(grey));
    CHECK(!picture::isGrey(card));
    image::Coded g;
    CHECK(image::encode(grey, 4000, g, error));
    int w = 0, h = 0, comps = 0;
    CHECK(picture::codestreamSize(g.bytes.data(), g.bytes.size(), w, h, comps));
    CHECK_EQ(comps, 1);
    Image greyBack;
    CHECK(image::decode(g.bytes.data(), g.bytes.size(), greyBack, error));
    CHECK_EQ(greyBack.channels, 1);

    // A tiny picture with room to spare still fits, and is near perfect.
    Image tiny = testCard(40, 30);
    image::Coded t;
    CHECK(image::encode(tiny, 50000, t, error));
    CHECK(t.bytes.size() < 50000);
    Image tinyBack;
    CHECK(image::decode(t.bytes.data(), t.bytes.size(), tinyBack, error));
    CHECK(psnr(tiny, tinyBack) > 30.0);

    // Too few bytes to say anything, or the wrong thing: no picture.
    Image none;
    CHECK(!image::decode(small.bytes.data(), 20, none, error));
    CHECK(!error.empty());
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!image::decode(junk, sizeof junk, none, error));
    CHECK(!image::encode(card, 100, small, error));
    // Bigger than the receiver shows: refused before it is decoded.
    CHECK(!image::decode(big.bytes.data(), big.bytes.size(), none, error, 320));

    // load() reads codestreams from disk too, and leaves JPEG to wxImage.
    Image loaded;
    CHECK(image::load(big.bytes.data(), big.bytes.size(), loaded, error) == image::Load::Done);
    CHECK_EQ(loaded.width, 640);
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0};
    CHECK(image::load(jpeg, sizeof jpeg, loaded, error) == image::Load::NotMine);
    CHECK(!image::versions().empty());

#if RAGTIME_TEST_WEBP
    // WebP, with transparency laid over white.
    std::vector<uint8_t> rgba(32 * 16 * 4);
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = 200;
        rgba[i + 1] = 30;
        rgba[i + 2] = 30;
        rgba[i + 3] = (i / 4) % 32 < 16 ? 255 : 0;
    }
    uint8_t* webp = nullptr;
    size_t webpBytes = WebPEncodeLosslessRGBA(rgba.data(), 32, 16, 32 * 4, &webp);
    CHECK(webpBytes > 0);
    Image fromWebp;
    CHECK(image::load(webp, webpBytes, fromWebp, error) == image::Load::Done);
    CHECK_EQ(fromWebp.width, 32);
    CHECK_EQ(fromWebp.height, 16);
    CHECK_EQ((int)fromWebp.row(0)[0], 200);
    CHECK_EQ((int)fromWebp.row(0)[20 * 3 + 1], 255);   // clear: white
    // Cut short, it is a WebP that can't be read, not someone else's.
    CHECK(image::load(webp, 20, fromWebp, error) == image::Load::Failed);
    WebPFree(webp);
#endif
}

} // namespace

int main()
{
    testFit();
    testDownscale();
    testOrient();
    testJpegOrientation();
    testAirtime();
    testNames();
    testCodec();
    return ragtime_test::finish();
}
