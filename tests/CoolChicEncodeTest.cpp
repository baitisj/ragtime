// image::coolchicEncode on a small picture with a short fit: the stream it
// writes decodes (whole and cut short), progress runs to the end, and a
// cancel stops it.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/image/CoolChic.h"
#include "ragtime/image/CoolChicEncode.h"
#include "ragtime/picture/Picture.h"

using namespace ragtime;

namespace
{

// A smooth picture with an edge or two: a gradient, a disc and a bar.
picture::Image testPicture(int w, int h, int channels)
{
    picture::Image img = picture::Image::blank(w, h, channels);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const bool disc = (x - w / 3) * (x - w / 3) + (y - h / 2) * (y - h / 2) < h * h / 9;
            const bool bar = x > 2 * w / 3 && x < 2 * w / 3 + 8;
            const int r = disc ? 220 : 40 + 150 * x / w;
            const int g = bar ? 30 : 60 + 120 * y / h;
            const int b = disc ? 60 : 180 - 100 * x / w;
            uint8_t* p = img.row(y) + (size_t)x * (size_t)channels;
            if (channels == 1)
                p[0] = (uint8_t)((r + g + b) / 3);
            else
            {
                p[0] = (uint8_t)r;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)b;
            }
        }
    return img;
}

double psnr(const picture::Image& a, const picture::Image& b)
{
    double err = 0.0;
    size_t n = 0;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++)
            for (int c = 0; c < 3; c++)
            {
                const int va = a.row(y)[(size_t)x * (size_t)a.channels +
                                        (size_t)(a.channels == 1 ? 0 : c)];
                const int vb = b.row(y)[(size_t)x * 3 + (size_t)c];
                err += (double)(va - vb) * (double)(va - vb);
                n++;
            }
    return 10.0 * std::log10(255.0 * 255.0 * (double)n / err);
}

image::CoolChicOptions shortFit()
{
    image::CoolChicOptions o;
    o.lambda = 0.01;
    o.iterations = 300;
    o.threads = 2;
    return o;
}

void testRoundTrip()
{
    const picture::Image img = testPicture(96, 64, 3);
    std::vector<double> seen;
    image::CoolChicResult r;
    std::string error;
    const bool ok = image::coolchicEncode(
        img, shortFit(),
        [&](double done) {
            seen.push_back(done);
            return true;
        },
        r, error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(!r.stream.empty());
    CHECK(!seen.empty());
    bool rising = true;
    for (size_t i = 1; i < seen.size(); i++)
        rising = rising && seen[i] >= seen[i - 1];
    CHECK(rising);
    CHECK(!seen.empty() && seen.back() == 1.0);

    picture::Image decoded;
    CHECK(image::coolchicDecode(r.stream.data(), r.stream.size(), decoded, error));
    CHECK_EQ(decoded.width, 96);
    CHECK_EQ(decoded.height, 64);
    CHECK_EQ(decoded.channels, 3);
    const double p = psnr(img, decoded);
    std::printf("96x64: %zu bytes, %.2f dB (fit says %.2f dB), %.1f s\n", r.stream.size(), p,
                r.psnr, r.seconds);
    CHECK(std::fabs(p - r.psnr) < 0.05);
    CHECK(p > 24.0);

    // A stream cut short inside the latents still shows a picture, from
    // the coarse grids.
    image::coolchic::Decoder dec;
    dec.feed(r.stream.data(), r.stream.size());
    const size_t latentStart = dec.latentStart();
    CHECK(latentStart > 0 && latentStart < r.stream.size());
    const size_t cut = latentStart + (r.stream.size() - latentStart) / 2;
    picture::Image partial;
    CHECK(image::coolchicDecode(r.stream.data(), cut, partial, error));
    CHECK_EQ(partial.width, 96);

    // The same seed writes the same stream.
    image::CoolChicResult again;
    CHECK(image::coolchicEncode(img, shortFit(), nullptr, again, error));
    CHECK(again.stream == r.stream);
}

void testGrey()
{
    const picture::Image img = testPicture(64, 64, 1);
    image::CoolChicOptions o = shortFit();
    o.iterations = 100;
    o.progressiveShare = 0.0;
    image::CoolChicResult r;
    std::string error;
    CHECK(image::coolchicEncode(img, o, nullptr, r, error));
    picture::Image decoded;
    CHECK(image::coolchicDecode(r.stream.data(), r.stream.size(), decoded, error));
    CHECK(std::fabs(psnr(img, decoded) - r.psnr) < 0.05);
}

void testCancelAndRefusals()
{
    const picture::Image img = testPicture(64, 64, 3);
    image::CoolChicResult r;
    std::string error;
    CHECK(!image::coolchicEncode(img, shortFit(), [](double done) { return done < 0.3; }, r,
                                 error));
    CHECK_EQ(error, std::string("cancelled"));
    CHECK(r.stream.empty());

    image::CoolChicOptions o = shortFit();
    o.preset = "huge";
    CHECK(!image::coolchicEncode(img, o, nullptr, r, error));
    CHECK(!image::coolchicEncode(picture::Image(), shortFit(), nullptr, r, error));
}

} // namespace

int main()
{
    testRoundTrip();
    testGrey();
    testCancelAndRefusals();
    return ragtime_test::finish();
}
