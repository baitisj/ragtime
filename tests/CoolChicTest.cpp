// Cool-chic pictures (docs/PICTURE_CODEC.md): the shape and its grids, the
// range coder, the test vectors in tests/data/coolchic/ (every latent, every
// partial cut, the pictures) and a fuzz run over damaged streams.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/image/CoolChic.h"
#include "ragtime/picture/Picture.h"
#include "../image/src/coolchic/Arm.h"
#include "../image/src/coolchic/RangeCoder.h"
#include "../image/src/coolchic/RangeTables.h"
#include "../image/src/coolchic/Stream.h"

using namespace ragtime;
using namespace ragtime::image;
using coolchic::Decoder;

namespace
{

const std::string DATA = std::string(RAGTIME_SOURCE_DIR) + "/tests/data/coolchic/";

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// tiny.rcc.latents.txt: "grid <index> <h> <w>", then the values row by row.
std::map<int, std::vector<int>> readLatents(const std::string& path)
{
    std::map<int, std::vector<int>> out;
    std::ifstream f(path);
    std::string line;
    int grid = -1;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream is(line);
        if (line.compare(0, 4, "grid") == 0)
        {
            std::string word;
            is >> word >> grid;
            continue;
        }
        int v;
        while (is >> v)
            out[grid].push_back(v);
    }
    return out;
}

// A binary PPM (P6, 255).
picture::Image readPpm(const std::string& path)
{
    picture::Image im;
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    int maxval = 0;
    f >> magic >> im.width >> im.height >> maxval;
    f.get();
    im.channels = 3;
    im.pixels.resize(size_t(im.width) * size_t(im.height) * 3);
    f.read(reinterpret_cast<char*>(im.pixels.data()), std::streamsize(im.pixels.size()));
    if (magic != "P6" || maxval != 255 || !f)
        return picture::Image();
    return im;
}

// How far two pictures are apart: the largest difference and how many
// samples differ at all.
void compare(const picture::Image& a, const picture::Image& b, int& maxDiff, size_t& differ)
{
    maxDiff = 0;
    differ = 0;
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
    {
        maxDiff = 256;
        return;
    }
    for (size_t i = 0; i < a.pixels.size(); i++)
    {
        const int d = std::abs(int(a.pixels[i]) - int(b.pixels[i]));
        maxDiff = std::max(maxDiff, d);
        differ += d != 0;
    }
}

// The spec's range encoder, for round trips (the library's own is the
// encoder's work).
class TestEncoder
{
public:
    void encode(int value, int m, int s)
    {
        const int i = value + coolchic::SYMBOLS / 2;
        const uint64_t lo = coolchic::cumulative(i, m, s), hi = coolchic::cumulative(i + 1, m, s);
        const uint64_t r = range_ >> coolchic::FREQ_BITS;
        add(r * lo);
        range_ = r * (hi - lo);
        while (range_ < (uint64_t(1) << 56))
        {
            range_ <<= 8;
            shiftLow();
        }
    }

    std::vector<uint8_t> finish()
    {
        for (int bits = 64; bits >= 0; bits--)
        {
            // v = low rounded up to a multiple of 2^bits, as a 65-bit value.
            const uint64_t mask = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
            uint64_t v = low_ + mask;
            int vCarry = carry_ + (v < low_);
            v &= ~mask;
            if (bits == 64)
                v = 0;
            // v < low + range (65-bit compare)
            uint64_t end = low_ + range_;
            int endCarry = carry_ + (end < low_);
            if (vCarry < endCarry || (vCarry == endCarry && v < end))
            {
                low_ = v;
                carry_ = vCarry;
                break;
            }
        }
        for (int i = 0; i < 9; i++)
            shiftLow();
        std::vector<uint8_t> run(out_.begin() + 1, out_.end());
        while (!run.empty() && run.back() == 0)
            run.pop_back();
        return run;
    }

private:
    void add(uint64_t v)
    {
        const uint64_t before = low_;
        low_ += v;
        carry_ += low_ < before;
    }

    void shiftLow()
    {
        if (low_ < (uint64_t(0xFF) << 56) || carry_)
        {
            const int carry = carry_;
            uint8_t temp = cache_;
            do
            {
                out_.push_back(uint8_t(temp + carry));
                temp = 0xFF;
            } while (--cacheSize_ != 0);
            cache_ = uint8_t(low_ >> 56);
        }
        cacheSize_++;
        low_ = (low_ & ((uint64_t(1) << 56) - 1)) << 8;
        carry_ = 0;
    }

    uint64_t low_ = 0;
    int carry_ = 0;
    uint64_t range_ = ~uint64_t(0);
    uint8_t cache_ = 0;
    uint64_t cacheSize_ = 1;
    std::vector<uint8_t> out_;
};

void testShape()
{
    coolchic::Architecture a;
    CHECK(coolchic::preset("lop", 480, 640, a));
    coolchic::Architecture none;
    CHECK(!coolchic::preset("nope", 480, 640, none));
    std::string why;
    CHECK(coolchic::valid(a, why));
    const std::vector<coolchic::Grid> g = coolchic::grids(a);
    CHECK_EQ(g.size(), size_t(10));
    // L0 L1 L2 L3 L4 H4 L5 H5 L6 H6
    const int down[10] = {0, 1, 2, 3, 4, 4, 5, 5, 6, 6};
    const bool hyper[10] = {false, false, false, false, false, true, false, true, false, true};
    const int ifce[10] = {9, 8, 7, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < g.size() && i < 10; i++)
    {
        CHECK_EQ(g[i].downsampling, down[i]);
        CHECK_EQ(g[i].hyper, hyper[i]);
        CHECK_EQ(g[i].ifceInputs, ifce[i]);
    }
    CHECK_EQ(g[0].height, 480);
    CHECK_EQ(g[0].width, 640);
    CHECK_EQ(g[8].height, 8);       // ceil(480 / 64)
    CHECK_EQ(g[8].width, 10);

    // lop's weights: about 0.4 kB of them.
    CHECK_EQ(coolchic::parameterCount(a, coolchic::ARM, false), size_t(2 * 64 + 16 + 16));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::ARM, true), size_t(8 + 8 + 2 + 2));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::IFCE, false), size_t(2 * (9 + 8 + 7)));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::IFCE, true), size_t(6));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::UPSAMPLING, false), size_t(6 * 4 + 6 * 4));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::UPSAMPLING, true), size_t(12));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::SYNTHESIS, false),
             size_t(9 + 21 + 56 + 24 + 81));
    CHECK_EQ(coolchic::parameterCount(a, coolchic::SYNTHESIS, true), size_t(3 + 3 + 8 + 3 + 3));

    // Past the caps.
    coolchic::Architecture big = a;
    big.width = coolchic::MAX_SIDE + 1;
    CHECK(!coolchic::valid(big, why));
    big = a;
    big.width = 2048;
    big.height = 2048;      // each side within the cap, but too many pixels
    CHECK(!coolchic::valid(big, why));
    big = a;
    big.synthesis[0].outputs = coolchic::MAX_SYNTHESIS_WIDTH + 1;
    CHECK(!coolchic::valid(big, why));
    big = a;
    big.synthesis[2].residual = true;
    big.synthesis[1].outputs = 4;   // a residual layer that changes width
    CHECK(!coolchic::valid(big, why));

    // The ARM's neighbours: left, above, above left, above right, two left,
    // two above.
    const auto offsets = coolchic::contextOffsets(6);
    CHECK_EQ(offsets.size(), size_t(6));
    const int expect[6][2] = {{0, -1}, {-1, 0}, {-1, -1}, {-1, 1}, {0, -2}, {-2, 0}};
    for (size_t i = 0; i < offsets.size() && i < 6; i++)
    {
        CHECK_EQ(offsets[i].first, expect[i][0]);
        CHECK_EQ(offsets[i].second, expect[i][1]);
    }
    CHECK_EQ(coolchic::contextOffsets(40).size(), size_t(40));
    CHECK_EQ(coolchic::shiftDown(-1, 4), int64_t(-1));
    CHECK_EQ(coolchic::shiftDown(-17, 4), int64_t(-2));
    CHECK_EQ(coolchic::shiftDown(17, 4), int64_t(1));
}

void testRangeCoder()
{
    // The tables are what their formulas say, to within a unit.
    for (int s = 0; s < coolchic::SCALE_TABLE_SIZE; s += 7)
    {
        const double k = std::ldexp(1.0, 32) / (256.0 * std::exp(-5.0 + s / 256.0) * std::log(2.0));
        CHECK(std::fabs(double(coolchic::SCALE_TABLE[s]) - k) <= 1.0);
    }
    for (int f = 0; f < coolchic::POW_TABLE_SIZE; f += 5)
        CHECK(std::fabs(double(coolchic::POW_TABLE[f]) - std::ldexp(1.0, 31) * std::exp2(-f / 1024.0)) <= 1.0);

    // Every symbol can be coded, whatever the model.
    bool ok = true;
    for (int m = 0; m < coolchic::MU_INDICES; m += 511)
        for (int s = 0; s < coolchic::SCALE_INDICES; s += 160)
        {
            ok = ok && coolchic::cumulative(0, m, s) == 0 &&
                 coolchic::cumulative(coolchic::SYMBOLS, m, s) == coolchic::FREQ_TOTAL;
            for (int i = 0; i < coolchic::SYMBOLS; i++)
                ok = ok && coolchic::cumulative(i + 1, m, s) > coolchic::cumulative(i, m, s);
        }
    CHECK(ok);

    // A round trip, and a cut run giving a true first part.
    std::mt19937 rng(7);
    std::vector<int> values, ms, ss;
    TestEncoder enc;
    for (int i = 0; i < 4000; i++)
    {
        const int m = int(rng() % coolchic::MU_INDICES), s = int(rng() % coolchic::SCALE_INDICES);
        // Mostly near mu, sometimes anywhere.
        int v = rng() % 4 == 0 ? int(rng() % 128) - 64 : (m - 16384) / 256 + int(rng() % 5) - 2;
        v = std::min(std::max(v, -64), 63);
        values.push_back(v);
        ms.push_back(m);
        ss.push_back(s);
        enc.encode(v, m, s);
    }
    const std::vector<uint8_t> run = enc.finish();
    coolchic::RangeDecoder dec;
    dec.start(run.data(), run.size());
    bool same = true;
    for (size_t i = 0; i < values.size(); i++)
        same = same && dec.decode(ms[i], ss[i]) == values[i];
    CHECK(same);

    const size_t cut = run.size() / 2;
    coolchic::RangeDecoder part;
    part.start(run.data(), cut);
    size_t good = 0;
    bool prefix = true;
    for (size_t i = 0; i < values.size(); i++)
    {
        const int v = part.decode(ms[i], ss[i]);
        if (part.pos() > cut)
            break;
        prefix = prefix && v == values[i];
        good++;
    }
    CHECK(prefix);
    CHECK(good > values.size() / 3);
}

// tiny.rcc, whole and cut, against the Python reference.
void testVectors()
{
    const std::vector<uint8_t> stream = readFile(DATA + "tiny.rcc");
    CHECK_EQ(stream.size(), size_t(884));
    const std::map<int, std::vector<int>> latents = readLatents(DATA + "tiny.rcc.latents.txt");
    CHECK_EQ(latents.size(), size_t(10));

    Decoder d;
    CHECK(d.feed(stream.data(), stream.size()) == Decoder::State::Complete);
    CHECK_EQ(d.latentStart(), size_t(450));
    CHECK_EQ(d.streamLength(), size_t(884));
    coolchic::Architecture lop;
    coolchic::preset("lop", 120, 160, lop);
    const coolchic::Architecture* a = d.architecture();
    CHECK(a != nullptr);
    if (a)
    {
        CHECK_EQ(a->height, 120);
        CHECK_EQ(a->width, 160);
        CHECK_EQ(a->spatialContext, lop.spatialContext);
        CHECK_EQ(a->ifceOutputs, lop.ifceOutputs);
        CHECK_EQ(a->synthesis.size(), lop.synthesis.size());
        CHECK_EQ(a->hyperLow, 4);
    }
    CHECK_EQ(d.gridCount(), size_t(10));
    size_t wrong = 0, total = 0;
    for (size_t g = 0; g < d.gridCount(); g++)
    {
        const auto it = latents.find(int(g));
        CHECK(it != latents.end());
        if (it == latents.end())
            continue;
        const std::vector<int8_t>& got = d.latents(g);
        CHECK_EQ(got.size(), it->second.size());
        for (size_t i = 0; i < got.size() && i < it->second.size(); i++)
            wrong += got[i] != it->second[i];
        total += got.size();
    }
    CHECK_EQ(total, size_t(25712));
    CHECK_EQ(wrong, size_t(0));

    picture::Image im;
    CHECK(d.picture(im));
    const picture::Image want = readPpm(DATA + "expected/tiny.ppm");
    CHECK(!want.empty());
    int maxDiff = 0;
    size_t differ = 0;
    compare(im, want, maxDiff, differ);
    CHECK(maxDiff <= 1);
    CHECK(differ * 1000 <= want.pixels.size());

    // The header writer gives back the same bytes.
    std::string why;
    coolchic::StreamHeader header;
    CHECK(coolchic::readHeaders(stream.data(), stream.size(), header, why) == coolchic::Read::Done);
    std::vector<uint8_t> written;
    CHECK(coolchic::writeHeaders(header, written, why));
    CHECK(written == std::vector<uint8_t>(stream.begin(), stream.begin() + std::ptrdiff_t(header.weightsStart)));
    for (size_t n = 0; n < header.weightsStart; n++)
        CHECK(coolchic::readHeaders(stream.data(), n, header, why) == coolchic::Read::NeedMore);

    picture::Image im2;
    CHECK(coolchicDecode(stream.data(), stream.size(), im2, why));
    CHECK(im2.pixels == im.pixels);

    // Cut streams: tiny.manifest's good values, finest grid first.
    struct Cut
    {
        size_t bytes;
        size_t good[10];
    };
    const Cut cuts[] = {
        {530, {0, 0, 0, 300, 80, 80, 20, 20, 6, 6}},
        {663, {2133, 4800, 1200, 300, 80, 80, 20, 20, 6, 6}},
        {795, {7900, 4800, 1200, 300, 80, 80, 20, 20, 6, 6}},
    };
    for (const Cut& c : cuts)
    {
        Decoder p;
        CHECK(p.feed(stream.data(), c.bytes) == Decoder::State::Partial);
        for (size_t g = 0; g < 10 && g < p.gridCount(); g++)
            CHECK_EQ(p.decoded(g), c.good[g]);
        // The good values are the true ones, the rest zero.
        bool ok = true;
        for (size_t g = 0; g < p.gridCount(); g++)
        {
            const std::vector<int8_t>& v = p.latents(g);
            size_t nonzeroBad = 0;
            size_t match = 0;
            for (size_t i = 0; i < v.size(); i++)
            {
                if (v[i] == latents.at(int(g))[i])
                    match++;
                else if (v[i] != 0)
                    nonzeroBad++;
            }
            ok = ok && nonzeroBad == 0 && match >= p.decoded(g);
        }
        CHECK(ok);
    }
    {
        Decoder p;
        p.feed(stream.data(), 663);
        picture::Image part;
        CHECK(p.picture(part));
        const picture::Image want663 = readPpm(DATA + "expected/tiny-663.ppm");
        compare(part, want663, maxDiff, differ);
        CHECK(maxDiff <= 1);
        CHECK(differ * 1000 <= want663.pixels.size());
    }
    for (size_t n : {size_t(0), size_t(10), size_t(265), size_t(397), size_t(449)})
    {
        Decoder p;
        CHECK(p.feed(stream.data(), n) == Decoder::State::Waiting);
        picture::Image none;
        CHECK(!p.picture(none));
        CHECK(!coolchicDecode(stream.data(), n, none, why));
    }

    // Fed burst by burst, it decodes what a fresh decoder of the same first
    // part does, and ends whole.
    std::mt19937 rng(11);
    Decoder burst;
    size_t have = 0;
    bool same = true;
    while (have < stream.size())
    {
        have = std::min(stream.size(), have + 1 + rng() % 40);
        const Decoder::State s = burst.feed(stream.data(), have);
        Decoder fresh;
        CHECK(fresh.feed(stream.data(), have) == s);
        for (size_t g = 0; g < fresh.gridCount() && g < burst.gridCount(); g++)
            same = same && fresh.decoded(g) == burst.decoded(g) &&
                   fresh.latents(g) == burst.latents(g);
    }
    CHECK(same);
    CHECK(burst.state() == Decoder::State::Complete);
    picture::Image last;
    CHECK(burst.picture(last));
    CHECK(last.pixels == im.pixels);
}

// Damaged and made-up streams never crash or run away.
void testFuzz()
{
    const std::vector<uint8_t> stream = readFile(DATA + "tiny.rcc");
    std::mt19937 rng(2026);
    int failed = 0, pictures = 0;
    for (int round = 0; round < 1000; round++)
    {
        std::vector<uint8_t> s = stream;
        const int kind = int(rng() % 5);
        if (kind == 0)
            for (int k = 0, n = 1 + int(rng() % 4); k < n; k++)
                s[rng() % s.size()] = uint8_t(rng());
        else if (kind == 1)
            for (int k = 0, n = 1 + int(rng() % 8); k < n; k++)
                s[rng() % s.size()] ^= uint8_t(1u << (rng() % 8));
        else if (kind == 2)
            // In the headers and the weights, where it hurts most.
            s[rng() % 460] ^= uint8_t(1u << (rng() % 8));
        else if (kind == 3)
            s.resize(rng() % s.size());
        else
        {
            s.resize(rng() % 2000);
            for (uint8_t& b : s)
                b = uint8_t(rng());
        }
        Decoder d;
        const size_t split = s.empty() ? 0 : rng() % s.size();
        d.feed(s.data(), split);
        const Decoder::State st = d.feed(s.data(), s.size());
        failed += st == Decoder::State::Failed;
        const coolchic::Architecture* a = d.architecture();
        picture::Image im;
        // The synthesis on one round in four keeps the test quick.
        if (a && round % 4 == 0 && int64_t(a->width) * a->height <= 200000 && d.picture(im))
        {
            pictures++;
            CHECK_EQ(im.width, a->width);
            CHECK_EQ(im.pixels.size(), size_t(a->width) * size_t(a->height) * 3);
        }
    }
    std::printf("fuzz: %d refused, %d pictures\n", failed, pictures);
    CHECK(failed > 0);
    CHECK(pictures > 0);
}

} // namespace

int main()
{
    testShape();
    testRangeCoder();
    testVectors();
    testFuzz();
    return ragtime_test::finish();
}
