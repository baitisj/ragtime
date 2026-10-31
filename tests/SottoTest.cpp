// voice::Sotto, libsotto loaded at run time. With no libsotto to be found
// (CI has none) only the refusals are checked; with one (RAGTIME_SOTTO, or
// -DRAGTIME_SOTTO_TEST_LIB=... at configure time) a clip goes through the
// encoder and decoder and comes back at about its level.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "Check.h"
#include "ragtime/voice/Sotto.h"
#include "ragtime/voice/Voice.h"

using namespace ragtime;

namespace
{

// A vowel-like buzz: 120 Hz harmonics under a formant envelope, at an RMS
// of about level dBFS, with 200 ms of silence either side.
std::vector<int16_t> buzz(double seconds, double levelDbfs)
{
    const int rate = voice::SOTTO_RATE;
    std::vector<double> s((size_t)(seconds * rate), 0.0);
    size_t lead = rate / 5;
    double phase = 0.0;     // of the fundamental, so the pitch glides smoothly
    for (size_t i = lead; i + lead < s.size(); i++)
    {
        double t = (double)i / rate;
        double f0 = 120.0 + 15.0 * std::sin(2 * M_PI * 1.5 * t);
        phase += 2 * M_PI * f0 / rate;
        double v = 0.0;
        for (int h = 1; h * 120 < 3600; h++)
        {
            double f = h * 120.0;
            double amp = std::exp(-std::pow((f - 700.0) / 400.0, 2)) + 0.5 * std::exp(-std::pow((f - 1200.0) / 300.0, 2))
                         + 0.05;
            v += amp * std::sin(h * phase);
        }
        s[i] = v;
    }
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = lead; i + lead < s.size(); i++, n++) sum += s[i] * s[i];
    double rms = std::sqrt(sum / std::max<size_t>(1, n));
    double want = 32768.0 * std::pow(10.0, levelDbfs / 20.0);
    std::vector<int16_t> out(s.size());
    for (size_t i = 0; i < s.size(); i++) out[i] = (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, s[i] * want / rms)));
    return out;
}

double activeDbfs(const std::vector<int16_t>& x)
{
    // RMS over the 20 ms blocks within 20 dB of the loudest.
    std::vector<double> blocks;
    for (size_t i = 0; i + 160 <= x.size(); i += 160)
    {
        double sum = 0.0;
        for (size_t j = 0; j < 160; j++) sum += (double)x[i + j] * x[i + j];
        blocks.push_back(sum / 160.0);
    }
    double loud = 0.0;
    for (double b : blocks) loud = std::max(loud, b);
    double sum = 0.0;
    int n = 0;
    for (double b : blocks)
        if (b > loud / 100.0)
        {
            sum += b;
            n++;
        }
    return n ? 10.0 * std::log10(sum / n / (32768.0 * 32768.0)) : -120.0;
}

} // namespace

int main()
{
    // A file that isn't there, and one that is but isn't libsotto.
    {
        voice::Sotto none("/nonexistent/libsotto.so.0");
        CHECK(!none.available());
        CHECK(!none.info().why.empty());
        CHECK(!none.encoder());
        CHECK(!none.decoder());
    }
#ifndef _WIN32
    {
        voice::Sotto notSotto("libm.so.6");
        CHECK(!notSotto.available());
        CHECK(notSotto.info().why.find("missing") != std::string::npos);
    }
#endif
    CHECK(!voice::sottoCandidates().empty());

    voice::Sotto& lib = voice::Sotto::library();
    if (!lib.available())
    {
        std::printf("libsotto not found (%s): round trip skipped\n", lib.info().why.c_str());
        return ragtime_test::finish();
    }
    std::printf("loaded %s: %s\n", lib.info().path.c_str(), lib.info().format.c_str());
    CHECK_EQ((int)lib.info().codec, (int)voice::CODEC_SOTTO_V36);

    // 2 s in, in odd-sized pieces, as the deck feeds it.
    std::vector<int16_t> in = buzz(2.0, -26.0);
    auto enc = lib.encoder();
    CHECK(enc != nullptr);
    std::vector<voice::SottoFrame> frames;
    size_t at = 0;
    for (size_t piece : {37u, 160u, 500u, 1u, 999u})
    {
        size_t take = std::min(piece, in.size() - at);
        enc->push(in.data() + at, take, frames);
        at += take;
    }
    enc->push(in.data() + at, in.size() - at, frames);
    enc->flush(frames);
    CHECK_EQ(frames.size(), in.size() / voice::SOTTO_SAMPLES);
    // Spent after the flush.
    size_t before = frames.size();
    enc->push(in.data(), 320, frames);
    CHECK_EQ(frames.size(), before);

    // The silence either side is silence; the buzz is voiced near 120 Hz.
    CHECK(voice::isSilent(frames.front()));
    CHECK(voice::isSilent(frames.back()));
    const voice::Frame& mid = frames[frames.size() / 2];
    CHECK(!voice::isSilent(mid));
    std::printf("middle frame: %.0f Hz\n", voice::pitchHzOf(mid));
    CHECK(voice::pitchHzOf(mid) > 90.0 && voice::pitchHzOf(mid) < 160.0);

    // The decoder gives the level back, with the delay taken out.
    auto dec = lib.decoder();
    CHECK(dec != nullptr);
    std::vector<int16_t> out = dec->decodeClip(frames, {});
    CHECK_EQ(out.size(), frames.size() * voice::SOTTO_SAMPLES);
    double inDb = activeDbfs(in), outDb = activeDbfs(out);
    std::printf("active level in %.1f dBFS, out %.1f dBFS\n", inDb, outDb);
    CHECK(std::fabs(inDb - outDb) < 4.0);
    // The start stays quiet: no 20 ms shift of the buzz into the lead-in.
    double lead = 0.0;
    for (size_t i = 0; i < 800; i++) lead = std::max(lead, std::fabs((double)out[i]));
    CHECK(lead < 200.0);

    // Lost frames are covered and then go quiet.
    std::vector<uint8_t> good(frames.size(), 1);
    for (size_t i = 40; i < 60; i++) good[i] = 0;
    std::vector<int16_t> holed = dec->decodeClip(frames, good);
    double tail = 0.0;
    for (size_t i = 52 * 160; i < 58 * 160; i++) tail = std::max(tail, std::fabs((double)holed[i]));
    CHECK(tail < 50.0);

    return ragtime_test::finish();
}
