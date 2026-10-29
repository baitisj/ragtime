// image::coolchicEncode: the fit, the quantisation and the stream, one
// after the other. Ragtime's own glue (MIT); the pieces it calls are ported
// from Cool-chic 5.0.1 (BSD-3, LICENSE.txt beside this file).
#include "ragtime/image/CoolChicEncode.h"

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

// The share of the progress the fit takes; quantising and writing the rest.
constexpr double FIT_SHARE = 0.88;
constexpr double QUANTISED = 0.92;

} // namespace

bool coolchicEncode(const picture::Image& image, const CoolChicOptions& options,
                    const CoolChicProgress& progress, CoolChicResult& out, std::string& error)
{
    using namespace coolchic;
    const auto t0 = std::chrono::steady_clock::now();
    auto going = [&](double done) { return !progress || progress(done); };

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
    model.gain = gainForLambda(options.lambda);

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
    Trainer trainer(model, target, pool, options.lambda, prog, options.seed);
    std::vector<float> params;
    const FitProgress fitProgress = [&](double done) { return going(FIT_SHARE * done); };
    if (!trainer.fit(intraSchedule(options.iterations, options.lambda), params, fitProgress))
        return error = "cancelled", false;

    Quantiser q(model, target, trainer.net(), options.lambda);
    q.quantise(params);
    if (!going(QUANTISED))
        return error = "cancelled", false;
    q.rdoq(params, options.seed);
    const StepResult r = q.test(params);
    if (!going(1.0))
        return error = "cancelled", false;

    CoolChicResult result;
    if (!writeStream(model, params, trainer.net(), q, result.stream, error))
        return false;
    result.psnr = 10.0 * std::log10(1.0 / r.mse);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out = std::move(result);
    return true;
}

} // namespace image
} // namespace ragtime
