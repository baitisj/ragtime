// image::coolchicEncode: the fit, the quantisation and the stream, one
// after the other. Ragtime's own glue (MIT); the pieces it calls are ported
// from Cool-chic 5.0.1 (BSD-3, LICENSE.txt beside this file).
#include "ragtime/image/CoolChicEncode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "FitModel.h"
#include "FitNet.h"
#include "FitPool.h"
#include "FitQuant.h"
#include "FitTrain.h"
#include "FitWrite.h"
#include "ragtime/image/CoolChic.h"

namespace ragtime
{
namespace image
{

namespace
{

// The share of the progress the fit takes; quantising and writing the rest
// (and, when a stream came out too big, the short fits after it).
constexpr double FIT_SHARE = 0.85;
constexpr double QUANTISED = 0.88;
constexpr double WRITTEN = 0.90;

// Steering the rate to a budget (Ragtime's). On the Wonder Wheel at 640x480
// with progressive fitting, the latents took 0.071 bits a pixel at lambda
// 0.03 and 0.0246 at 0.13, so their bits go as lambda^-0.72; a first guess
// from that is within a factor of two or so for most pictures, and the fit
// steers from there at each validation.
constexpr double BPP_AT_REFERENCE = 0.071;
constexpr double REFERENCE_LAMBDA = 0.03;
constexpr double RATE_SLOPE = 0.72;
constexpr double LAMBDA_MIN = 0.0006;   // below Cool-chic's gain changes (0.0005)
constexpr double LAMBDA_MAX = 4.0;
// The headers, the grid table and the flushes, and about 0.8 bytes a
// network parameter (lop: 505 parameters, about 0.4 kB as sent).
constexpr double HEADER_BYTES = 40.0;
constexpr double BYTES_PER_PARAMETER = 0.8;
// The fit aims a little under the budget, as the rate it sees is the float
// ARM's estimate and the weights' cost is only known once quantised.
constexpr double AIM = 0.94;
// The rate early in the fit says little of where it ends: it climbs while
// the latents grow from nothing under heavy noise, then falls several
// times over (on the Wonder Wheel at 1.6 kB, to 4,000 bytes and then down
// to 350). Steering on it then sent lambda twentyfold too high. So lambda
// is steered only from STEER_FROM of the main phase, when the rate has
// settled, to STEER_UNTIL, gently: each validation moves it part of the
// way, at most by STEER_MOST.
constexpr double STEER_GAIN = 0.3;
constexpr double STEER_MOST = 1.25;
constexpr double STEER_FROM = 0.6;
constexpr double STEER_UNTIL = 0.85;
// A stream still over the budget gets this many short fits more.
constexpr int RETRIES = 2;

double clampLambda(double lambda)
{
    return std::max(LAMBDA_MIN, std::min(LAMBDA_MAX, lambda));
}

// The lambda whose latents take bytes for a picture of pixels.
double lambdaFor(double bytes, double pixels)
{
    const double bpp = std::max(1e-4, 8.0 * bytes / pixels);
    return clampLambda(REFERENCE_LAMBDA * std::pow(BPP_AT_REFERENCE / bpp, 1.0 / RATE_SLOPE));
}

picture::Image toImage(const std::vector<float>& rgb, int height, int width)
{
    picture::Image img = picture::Image::blank(width, height, 3);
    const size_t plane = (size_t)width * (size_t)height;
    if (rgb.size() < 3 * plane)
        return img;
    for (size_t i = 0; i < plane; i++)
        for (size_t c = 0; c < 3; c++)
        {
            const float v = std::max(0.0f, std::min(1.0f, rgb[c * plane + i]));
            img.pixels[i * 3 + c] = (uint8_t)std::lround(v * 255.0f);
        }
    return img;
}

} // namespace

bool coolchicEncode(const picture::Image& image, const CoolChicOptions& options,
                    const CoolChicProgress& progress, CoolChicResult& out, std::string& error,
                    const CoolChicPreview& preview)
{
    using namespace coolchic;
    const auto t0 = std::chrono::steady_clock::now();
    // Never backwards, though a stream too big fits again.
    double reached = 0.0;
    auto going = [&](double done) {
        reached = std::max(reached, done);
        return !progress || progress(reached);
    };

    if (image.empty() || (image.channels != 1 && image.channels != 3 && image.channels != 4) ||
        image.pixels.size() <
            (size_t)image.width * (size_t)image.height * (size_t)image.channels)
        return error = "not a picture", false;
    if (options.lambda <= 0.0 || options.iterations <= 0)
        return error = "bad options", false;
    Architecture shape;
    if (!preset(options.preset, image.height, image.width, shape))
        return error = "no preset " + options.preset, false;
    if (!valid(shape, error))
        return false;

    Arch arch;
    Model model;
    if (!archByName(options.preset, arch))
        return error = "no preset " + options.preset, false;
    if (!buildModel(arch, image.height, image.width, model, error))
        return false;

    // With a budget, the latents get what the headers and the weights
    // leave of it.
    const double pixels = (double)image.width * (double)image.height;
    const double networkParameters = (double)(model.count - model.latentCount());
    const double overhead = HEADER_BYTES + BYTES_PER_PARAMETER * networkParameters;
    const double budget = (double)options.maxBytes;
    if (options.maxBytes > 0 && budget * AIM <= overhead + 16.0)
        return error = "too few bytes for even the network's weights", false;
    double latentAim = budget * AIM - overhead;
    const double lambda0 = options.maxBytes > 0 ? lambdaFor(latentAim, pixels) : options.lambda;
    model.gain = gainForLambda(lambda0);

    // The picture as the fit takes it: [3][height][width] in [0, 1].
    Target target;
    target.height = image.height;
    target.width = image.width;
    const size_t plane = (size_t)image.width * (size_t)image.height;
    target.rgb.resize(3 * plane);
    for (size_t i = 0; i < plane; i++)
        for (size_t c = 0; c < 3; c++)
        {
            const size_t from = image.channels == 1 ? i : i * (size_t)image.channels + c;
            target.rgb[c * plane + i] = (float)image.pixels[from] / 255.0f;
        }

    Pool pool(options.threads);
    Progressive prog;
    prog.share = options.progressiveShare;
    prog.maxDropped = options.progressiveDropped;
    Trainer trainer(model, target, pool, lambda0, prog, options.seed);
    if (preview)
    {
        trainer.pictureSeconds = options.previewSeconds;
        trainer.onPicture = [&](const std::vector<float>& rgb) {
            preview(toImage(rgb, image.height, image.width));
        };
    }
    if (options.maxBytes > 0)
    {
        // At the main phase's later validations, lambda moves toward the
        // one whose rate fits: part of the way, as the rate lags it.
        trainer.steer = [&](const StepResult& r, int phase, double phaseDone) {
            const double lambda = trainer.lambda();
            if (phase != 0 || phaseDone < STEER_FROM || phaseDone > STEER_UNTIL || r.rateBits <= 0.0)
                return lambda;
            const double ratio = r.rateBits / 8.0 / latentAim;
            const double factor = std::pow(ratio, STEER_GAIN / RATE_SLOPE);
            return clampLambda(lambda * std::max(1.0 / STEER_MOST, std::min(STEER_MOST, factor)));
        };
    }
    std::vector<float> params;
    const FitProgress fitProgress = [&](double done) { return going(FIT_SHARE * done); };
    if (!trainer.fit(intraSchedule(options.iterations, lambda0), params, fitProgress))
        return error = "cancelled", false;

    CoolChicResult result;
    StepResult r;
    for (int attempt = 0;; attempt++)
    {
        const double share = attempt == 0 ? 0.0 : (1.0 - WRITTEN) * attempt / (RETRIES + 1);
        Quantiser q(model, target, trainer.net(), trainer.lambda());
        q.going = [&]() { return going(FIT_SHARE + share); };
        if (!q.quantise(params) || !going(QUANTISED + share))
            return error = "cancelled", false;
        if (!q.rdoq(params, options.seed))
            return error = "cancelled", false;
        r = q.test(params);
        if (!writeStream(model, params, trainer.net(), q, result.stream, error))
            return false;
        if (options.maxBytes == 0 || result.stream.size() <= options.maxBytes)
            break;
        if (attempt == RETRIES)
        {
            error = "it came to " + std::to_string(result.stream.size()) + " bytes, over the " +
                    std::to_string(options.maxBytes) + " allowed";
            return false;
        }
        // Too big: the weights' real cost is known now. Aim the latents
        // at what the budget leaves, a little lower each time, and fit on
        // from here a while at the lambda for that.
        const double weights = (double)result.stream.size() - r.rateBits / 8.0;
        latentAim = std::max(16.0, budget * (AIM - 0.04 * (attempt + 1)) - weights);
        const double ratio = r.rateBits / 8.0 / latentAim;
        trainer.setLambda(clampLambda(trainer.lambda() * std::pow(ratio, 1.0 / RATE_SLOPE)));
        Phase settle;
        settle.lr = 0.002;
        settle.maxItr = 300;
        settle.scheduleLr = true;
        settle.temperature[0] = 0.12f;
        settle.temperature[1] = 0.08f;
        settle.noise[0] = settle.noise[1] = 0.15f;
        Phase ste;
        ste.lr = 1e-4;
        ste.maxItr = 150;
        ste.scheduleLr = true;
        ste.hardRound = true;
        const double from = WRITTEN + share, span = (1.0 - WRITTEN) / (RETRIES + 1);
        int steps = 0;
        const FitProgress more = [&](double) {
            steps++;
            return going(from + span * std::min(1.0, steps / 450.0));
        };
        if (!trainer.fitPhases({settle, ste}, params, more))
            return error = "cancelled", false;
    }
    if (!going(1.0))
        return error = "cancelled", false;
    result.lambda = trainer.lambda();
    result.psnr = 10.0 * std::log10(1.0 / r.mse);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out = std::move(result);
    return true;
}

} // namespace image
} // namespace ragtime
