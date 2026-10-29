// The Cool-chic encoder's hand-written backward passes against PyTorch's
// autograd on Cool-chic 5.0.1's own modules (tests/data/coolchic/fit/, made
// by tools/codecs/coolchic_fit_check.py). Finite differences were tried
// first, but the soft-round, the ReLUs, the clamp and the single-precision
// rate make the loss too rough for them to say anything sharper.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "FitModel.h"
#include "FitNet.h"
#include "FitOptim.h"
#include "FitPool.h"

using namespace ragtime::image::coolchic;

namespace
{

const std::string DATA = std::string(RAGTIME_SOURCE_DIR) + "/tests/data/coolchic/fit/";

bool readFloats(const std::string& path, std::vector<float>& v)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(v.size() * sizeof(float)));
    return (bool)f;
}

// One vector: the C++ side's parameters and target, PyTorch's loss and
// gradient for them (soft-round at temperature 0.3, no noise, lambda 0.02).
void checkAgainstTorch(const std::string& name, const std::string& archName, int h, int w,
                       int drop, int threads)
{
    Arch arch;
    CHECK(archByName(archName, arch));
    Model m;
    std::string error;
    CHECK(buildModel(arch, h, w, m, error));
    const std::string dir = DATA + name + "/";

    // The tensors must be where the vector says.
    std::ifstream names(dir + "names.txt");
    std::string line;
    int count = 0;
    while (std::getline(names, line))
    {
        std::istringstream in(line);
        std::string tname;
        size_t offset = 0, size = 0;
        in >> tname >> offset >> size;
        const int t = m.tensorIndex(tname);
        CHECK(t >= 0);
        if (t >= 0)
        {
            CHECK_EQ(m.tensors[(size_t)t].offset, offset);
            CHECK_EQ(m.tensors[(size_t)t].size, size);
        }
        count++;
    }
    CHECK_EQ((size_t)count, m.tensors.size());

    std::vector<float> params(m.count), torchGrads(m.count);
    Target target;
    target.height = h;
    target.width = w;
    target.rgb.resize((size_t)3 * (size_t)h * (size_t)w);
    CHECK(readFloats(dir + "params.bin", params));
    CHECK(readFloats(dir + "target.bin", target.rgb));
    CHECK(readFloats(dir + "torch_grads.bin", torchGrads));
    double torchLoss = 0.0, torchRate = 0.0;
    std::ifstream(dir + "torch_loss.txt") >> torchLoss >> torchRate;

    Pool pool(threads);
    Net net(m, pool);
    StepConfig cfg;
    cfg.temperature = 0.3f;
    cfg.noise = 0.0f;
    cfg.lambda = 0.02;
    cfg.dropFinest = drop;
    std::vector<float> grads(m.count);
    const StepResult r = net.step(params.data(), grads.data(), cfg, target);
    CHECK(std::fabs(r.loss - torchLoss) <= 1e-6 * torchLoss);
    CHECK(std::fabs(r.rateBits - torchRate) <= 1e-5 * torchRate);

    for (const Tensor& t : m.tensors)
    {
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i < t.size; i++)
        {
            const double d = (double)grads[t.offset + i] - torchGrads[t.offset + i];
            num += d * d;
            den += (double)torchGrads[t.offset + i] * torchGrads[t.offset + i];
        }
        const double rel = den > 0.0 ? std::sqrt(num / den) : std::sqrt(num);
        if (rel > 1e-3)
            std::fprintf(stderr, "%s: %s differs from PyTorch by %.2e\n", name.c_str(),
                         t.name.c_str(), rel);
        CHECK(rel <= 1e-3);
    }
}

// The optimiser against Cool-chic's SOAP (tools/codecs/coolchic_soap_check.py):
// the same tensors through the same gradients end in the same place.
void checkSoap()
{
    const std::string dir = DATA + "soap/";
    Model m;
    std::ifstream shapes(dir + "shapes.txt");
    std::string line;
    while (std::getline(shapes, line))
    {
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        Tensor t;
        t.name = kind + std::to_string(m.tensors.size());
        t.module = kind == "latent" ? Module::Latent : Module::Synthesis;
        int d;
        t.size = 1;
        while (in >> d)
        {
            t.shape.push_back(d);
            t.size *= (size_t)d;
        }
        t.offset = m.count;
        m.count += t.size;
        m.tensors.push_back(t);
    }
    CHECK_EQ(m.tensors.size(), (size_t)7);
    const int steps = 25;
    std::vector<float> params(m.count), grads((size_t)steps * m.count), final(m.count);
    CHECK(readFloats(dir + "init.bin", params));
    CHECK(readFloats(dir + "grads.bin", grads));
    CHECK(readFloats(dir + "final.bin", final));
    OptimConfig cfg;
    cfg.lr = 0.01;
    cfg.preconditionFrequency = 3;
    Optimizer opt(m, cfg);
    for (int s = 0; s < steps; s++)
        opt.step(params.data(), grads.data() + (size_t)s * m.count);
    double worst = 0.0;
    for (size_t i = 0; i < m.count; i++)
        worst = std::max(worst, (double)std::fabs(params[i] - final[i]));
    if (worst > 1e-5)
        std::fprintf(stderr, "soap: %.2e from Cool-chic's\n", worst);
    CHECK(worst <= 1e-5);
}

} // namespace

int main()
{
    // Odd sizes, so every grid is cropped; one thread and several.
    checkAgainstTorch("lop-37x45-drop0", "lop", 37, 45, 0, 1);
    checkAgainstTorch("lop-37x45-drop0", "lop", 37, 45, 0, 3);
    checkAgainstTorch("mop-33x61-drop1", "mop", 33, 61, 1, 2);
    checkSoap();
    return ragtime_test::finish();
}
