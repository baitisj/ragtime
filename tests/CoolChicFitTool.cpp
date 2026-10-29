// ragtime_coolchic_fit: a developer's tool for the Cool-chic encoder, not a
// test. tools/codecs/coolchic_fit_check.py runs it beside Cool-chic's own
// PyTorch code to check the two agree.
//
//   ragtime_coolchic_fit grad ARCH H W SEED DIR [DROP]
//       random parameters and target; writes DIR/names.txt (name offset
//       size), params.bin, target.bin, grads.bin and loss.txt
//   ragtime_coolchic_fit eval ARCH DIR
//       reads DIR/params.bin and target.bin back and prints the test loss
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "FitModel.h"
#include "FitNet.h"
#include "FitPool.h"
#include "FitQuant.h"
#include "FitTrain.h"
#include "FitWrite.h"
#include "ragtime/image/CoolChic.h"
#include "ragtime/picture/Picture.h"

using namespace ragtime::image::coolchic;

namespace
{

void writeFloats(const std::string& path, const std::vector<float>& v)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(v.size() * sizeof(float)));
}

bool readFloats(const std::string& path, std::vector<float>& v)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(v.size() * sizeof(float)));
    return (bool)f;
}

// A binary PPM (P6, 8 bits) as planar RGB in [0, 1].
bool readPpm(const std::string& path, Target& t)
{
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, maxv = 0;
    f >> magic >> w >> h >> maxv;
    f.get();
    if (!f || magic != "P6" || maxv != 255 || w <= 0 || h <= 0)
        return false;
    std::vector<unsigned char> px((size_t)w * (size_t)h * 3);
    f.read(reinterpret_cast<char*>(px.data()), (std::streamsize)px.size());
    if (!f)
        return false;
    t.width = w;
    t.height = h;
    t.rgb.resize(px.size());
    const size_t plane = (size_t)w * (size_t)h;
    for (size_t i = 0; i < plane; i++)
        for (size_t c = 0; c < 3; c++)
            t.rgb[c * plane + i] = (float)px[i * 3 + c] / 255.0f;
    return true;
}

void writePpm(const std::string& path, const std::vector<float>& rgb, int h, int w)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    const size_t plane = (size_t)w * (size_t)h;
    for (size_t i = 0; i < plane; i++)
        for (size_t c = 0; c < 3; c++)
            f.put((char)(unsigned char)std::lround(rgb[c * plane + i] * 255.0f));
}

// The networks quantised (with RDOQ), and what the stream would hold.
template <typename Clock>
void quantiseAndReport(const Model& m, const Target& target, Net& net, double lambda,
                       std::vector<float>& params, const std::string& dir, Clock seconds)
{
    Quantiser q(m, target, net, lambda);
    q.quantise(params);
    StepResult r = q.test(params);
    std::printf("%7.1f s  quantised: psnr %.3f dB, latents %.0f B, networks %.0f B\n", seconds(),
                10.0 * std::log10(1.0 / r.mse), r.rateBits / 8.0, q.networkBits(params) / 8.0);
    q.rdoq(params, 7);
    r = q.test(params);
    std::printf("%7.1f s  rdoq: psnr %.3f dB, latents %.0f B, networks %.0f B\n", seconds(),
                10.0 * std::log10(1.0 / r.mse), r.rateBits / 8.0, q.networkBits(params) / 8.0);
    const char* names[] = {"arm", "ifce", "upsampling", "synthesis"};
    for (int n = 0; n < NETS; n++)
        std::printf("  %-10s step 2^%d / 2^%d, orders %d / %d\n", names[n], q.quant(n).weightShift,
                    q.quant(n).biasShift, q.quant(n).weightOrder, q.quant(n).biasOrder);
    writeFloats(dir + "/quantised.bin", params);
    writePpm(dir + "/quantised.ppm", net.picture(), target.height, target.width);

    // The stream, and the decoder's picture of it.
    std::vector<uint8_t> stream;
    std::string error;
    size_t clamped = 0;
    if (!writeStream(m, params, net, q, stream, error, &clamped))
    {
        std::printf("can't write the stream: %s\n", error.c_str());
        return;
    }
    std::ofstream(dir + "/stream.rcc", std::ios::binary)
        .write(reinterpret_cast<const char*>(stream.data()), (std::streamsize)stream.size());
    ragtime::picture::Image decoded;
    if (!ragtime::image::coolchicDecode(stream.data(), stream.size(), decoded, error))
    {
        std::printf("%7.1f s  stream %zu B, clamped %zu, won't decode: %s\n", seconds(),
                    stream.size(), clamped, error.c_str());
        return;
    }
    const size_t plane = (size_t)target.height * (size_t)target.width;
    double err = 0.0, diff = 0.0;
    for (size_t i = 0; i < plane; i++)
        for (int c = 0; c < 3; c++)
        {
            const double d = decoded.pixels[i * 3 + (size_t)c] / 255.0;
            const double t = target.rgb[(size_t)c * plane + i];
            err += (d - t) * (d - t);
            diff = std::max(diff, std::fabs(d - (double)net.picture()[(size_t)c * plane + i]));
        }
    std::printf("%7.1f s  stream %zu B (%.4f bpp), clamped %zu: decoded psnr %.3f dB, "
                "worst difference from the fit's %.0f/255\n",
                seconds(), stream.size(), 8.0 * (double)stream.size() / (double)plane, clamped,
                10.0 * std::log10(3.0 * (double)plane / err), diff * 255.0);

    // The picture from a stream cut at the end of each grid, coarsest first.
    ragtime::image::coolchic::Decoder dec;
    dec.feed(stream.data(), stream.size());
    size_t at = dec.latentStart();
    std::vector<size_t> lengths;
    for (size_t k = 0; k < dec.gridCount(); k++)
    {
        size_t n = 0;
        int shift = 0;
        uint8_t b = 0;
        do
        {
            b = stream[at++];
            n |= (size_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        lengths.push_back(n);
    }
    size_t end = at;
    for (size_t k = 0; k < lengths.size(); k++)
    {
        end += lengths[k];
        ragtime::picture::Image cut;
        if (!ragtime::image::coolchicDecode(stream.data(), end, cut, error))
            continue;
        double e = 0.0;
        for (size_t i = 0; i < plane; i++)
            for (int c = 0; c < 3; c++)
            {
                const double d = cut.pixels[i * 3 + (size_t)c] / 255.0 -
                                 target.rgb[(size_t)c * plane + i];
                e += d * d;
            }
        std::printf("  cut after grid %zu (%zu B): psnr %.3f dB\n", lengths.size() - 1 - k, end,
                    10.0 * std::log10(3.0 * (double)plane / e));
    }
}

int usage()
{
    std::fprintf(stderr, "usage: ragtime_coolchic_fit grad ARCH H W SEED DIR [DROP]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    const std::string cmd = argv[1];
    if (cmd == "grad" && argc >= 7)
    {
        Arch arch;
        if (!archByName(argv[2], arch))
            return usage();
        const int h = std::atoi(argv[3]);
        const int w = std::atoi(argv[4]);
        const uint64_t seed = std::strtoull(argv[5], nullptr, 10);
        const std::string dir = argv[6];
        const int drop = argc >= 8 ? std::atoi(argv[7]) : 0;
        Model m;
        std::string error;
        if (!buildModel(arch, h, w, m, error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        Random rng(seed);
        std::vector<float> params;
        initParams(m, seed, params);
        // Move every parameter off its initial value, so none is trivial.
        for (const Tensor& t : m.tensors)
            for (size_t i = 0; i < t.size; i++)
                params[t.offset + i] +=
                    (t.module == TensorKind::Latent ? 0.1f : 0.05f) * rng.normal();
        params[m.tensors[(size_t)m.armLayers.back().bias].offset + 1] += 4.0f;
        Target target;
        target.height = h;
        target.width = w;
        target.rgb.resize((size_t)3 * (size_t)h * (size_t)w);
        for (float& v : target.rgb)
            v = (float)rng.uniform();
        for (const ConvRef& c : m.synLayers)
            if (&c == &m.synLayers.back())
                for (int o = 0; o < c.layer.out; o++)
                    params[m.tensors[(size_t)c.bias].offset + (size_t)o] += 0.5f;

        Pool pool(1);
        Net net(m, pool);
        StepConfig cfg;
        cfg.temperature = 0.3f;
        cfg.noise = 0.0f;
        cfg.lambda = 0.02;
        cfg.dropFinest = drop;
        std::vector<float> grads(m.count);
        const StepResult r = net.step(params.data(), grads.data(), cfg, target);

        std::ofstream names(dir + "/names.txt");
        for (const Tensor& t : m.tensors)
            names << t.name << " " << t.offset << " " << t.size << "\n";
        writeFloats(dir + "/params.bin", params);
        writeFloats(dir + "/target.bin", target.rgb);
        writeFloats(dir + "/grads.bin", grads);
        std::ofstream loss(dir + "/loss.txt");
        char buf[200];
        std::snprintf(buf, sizeof buf, "%.9g %.9g %.9g\n", r.loss, r.mse, r.rateBits);
        loss << buf;
        std::printf("%s", buf);
        return 0;
    }
    if (cmd == "eval" && argc >= 6)
    {
        Arch arch;
        if (!archByName(argv[2], arch))
            return usage();
        const int h = std::atoi(argv[3]);
        const int w = std::atoi(argv[4]);
        const std::string dir = argv[5];
        Model m;
        std::string error;
        if (!buildModel(arch, h, w, m, error))
            return 1;
        std::vector<float> params(m.count);
        Target target;
        target.height = h;
        target.width = w;
        target.rgb.resize((size_t)3 * (size_t)h * (size_t)w);
        if (!readFloats(dir + "/params.bin", params) || !readFloats(dir + "/target.bin", target.rgb))
            return 1;
        Pool pool(1);
        Net net(m, pool);
        StepConfig cfg;
        cfg.hardRound = true;
        cfg.roundOutput = true;
        cfg.lambda = 0.02;
        const StepResult r = net.step(params.data(), nullptr, cfg, target);
        std::printf("%.9g %.9g %.9g\n", r.loss, r.mse, r.rateBits);
        return 0;
    }
    if (cmd == "fit" && argc >= 8)
    {
        // fit ARCH LAMBDA ITERATIONS THREADS IN.ppm OUTDIR [SHARE]
        Arch arch;
        if (!archByName(argv[2], arch))
            return usage();
        const double lambda = std::atof(argv[3]);
        const int iterations = std::atoi(argv[4]);
        const int threads = std::atoi(argv[5]);
        Target target;
        if (!readPpm(argv[6], target))
        {
            std::fprintf(stderr, "can't read %s\n", argv[6]);
            return 1;
        }
        const std::string dir = argv[7];
        Progressive prog;
        if (argc >= 9)
            prog.share = std::atof(argv[8]);
        Model m;
        std::string error;
        if (!buildModel(arch, target.height, target.width, m, error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        m.gain = gainForLambda(lambda);
        Pool pool(threads);
        Trainer trainer(m, target, pool, lambda, prog, 1);
        const double pixels = (double)target.height * target.width;
        const auto t0 = std::chrono::steady_clock::now();
        auto seconds = [&]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        };
        trainer.onValidation = [&](int step, const StepResult& r) {
            std::printf("%7.1f s  step %5d  loss %.6f  psnr %6.3f dB  latents %7.0f B (%.4f bpp)\n",
                        seconds(), step, r.loss, 10.0 * std::log10(1.0 / r.mse),
                        r.rateBits / 8.0, r.rateBits / pixels);
            std::fflush(stdout);
        };
        const Schedule schedule = intraSchedule(iterations, lambda);
        std::vector<float> params;
        trainer.fit(schedule, params, [](double) { return true; });
        const StepResult r = trainer.test(params);
        std::printf("fitted in %.1f s: psnr %.3f dB, latents %.0f B (float ARM)\n", seconds(),
                    10.0 * std::log10(1.0 / r.mse), r.rateBits / 8.0);
        writeFloats(dir + "/params.bin", params);
        writePpm(dir + "/decoded.ppm", trainer.net().picture(), target.height, target.width);
        quantiseAndReport(m, target, trainer.net(), lambda, params, dir, seconds);
        return 0;
    }
    if (cmd == "quant" && argc >= 7)
    {
        // quant ARCH LAMBDA THREADS IN.ppm DIR: DIR/params.bin from fit, again
        Arch arch;
        if (!archByName(argv[2], arch))
            return usage();
        const double lambda = std::atof(argv[3]);
        Target target;
        if (!readPpm(argv[5], target))
            return 1;
        const std::string dir = argv[6];
        Model m;
        std::string error;
        if (!buildModel(arch, target.height, target.width, m, error))
            return 1;
        m.gain = gainForLambda(lambda);
        std::vector<float> params(m.count);
        if (!readFloats(dir + "/params.bin", params))
            return 1;
        Pool pool(std::atoi(argv[4]));
        Net net(m, pool);
        const auto t0 = std::chrono::steady_clock::now();
        auto seconds = [&]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        };
        quantiseAndReport(m, target, net, lambda, params, dir, seconds);
        return 0;
    }
    if (cmd == "bench" && argc >= 6)
    {
        Arch arch;
        if (!archByName(argv[2], arch))
            return usage();
        const int h = std::atoi(argv[3]);
        const int w = std::atoi(argv[4]);
        const int threads = std::atoi(argv[5]);
        Model m;
        std::string error;
        if (!buildModel(arch, h, w, m, error))
            return 1;
        std::vector<float> params;
        initParams(m, 1, params);
        Random rng(3);
        for (size_t i = 0; i < m.latentCount(); i++)
            params[i] = 0.05f * rng.normal();
        Target target;
        target.height = h;
        target.width = w;
        target.rgb.assign((size_t)3 * (size_t)h * (size_t)w, 0.5f);
        Pool pool(threads);
        Net net(m, pool);
        StepConfig cfg;
        cfg.noise = 0.2f;
        std::vector<float> grads(m.count);
        net.step(params.data(), grads.data(), cfg, target);
        const auto t0 = std::chrono::steady_clock::now();
        const int n = 10;
        for (int i = 0; i < n; i++)
            net.step(params.data(), grads.data(), cfg, target);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("%.1f ms per step (%d threads):", ms, threads);
        const char* parts[] = {"quantise+ifce", "arm", "ups", "syn", "syn-back", "ups-back", "ctx-back", "ifce-back", "latent", "quantise"};
        for (int i = 0; i < 10; i++)
            std::printf(" %s %.1f", parts[i], net.times[(size_t)i] / (n + 1));
        std::printf("\n");
        return 0;
    }
    return usage();
}
