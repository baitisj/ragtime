// RNNoise as the voice chain's noise reducer (denoise/): its delay, what it
// does to plain noise, and the chain's Noise stage with it, on synthetic
// voices in noise. With -DRAGTIME_RNNOISE=OFF it checks the stand-in.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Check.h"
#include "VoiceSynth.h"
#include "ragtime/denoise/RnNoise.h"
#include "ragtime/voice/Chain.h"

using namespace ragtime;
using namespace ragtime::voice;
using namespace voice_synth;

namespace
{

std::vector<float> denoiseAll(Denoiser& d, std::vector<float> x)
{
    x.resize(x.size() / Denoiser::FRAME * Denoiser::FRAME);
    for (size_t i = 0; i < x.size(); i += Denoiser::FRAME)
        d.process(x.data() + i);
    return x;
}

void testDelay()
{
    // The output lines up with the input LATENCY samples later.
    auto d = denoise::makeRnNoise();
    std::vector<float> x = speech(48000, 4.0, 21);
    setActiveLevel(x, 48000, -26);
    addNoise(x, -60, 21);
    std::vector<float> y = denoiseAll(*d, x);
    int best = -1;
    double bestCorr = -1;
    for (int lag = 0; lag <= 1200; lag += 1)
    {
        double c = 0;
        for (size_t i = 48000; i + (size_t)lag < y.size(); i += 3)
            c += (double)x[i] * y[i + (size_t)lag];
        if (c > bestCorr)
        {
            bestCorr = c;
            best = lag;
        }
    }
    std::fprintf(stderr, "RNNoise delay: %d samples (says %d)\n", best, d->latency());
    CHECK(std::abs(best - d->latency()) <= 2);
}

void testNoise()
{
    // A fan's noise alone is taken well down once it has listened a moment.
    // (Plain white noise takes it several seconds: RNNoise learnt on real
    // noises, which fall with frequency.)
    for (double db : {-50.0, -35.0})
    {
        auto d = denoise::makeRnNoise();
        std::vector<float> x(48000 * 4, 0.0f);
        addFanNoise(x, db, 22);
        std::vector<float> y = denoiseAll(*d, x);
        double out = rmsDb(y, 48000, y.size());
        std::fprintf(stderr, "RNNoise: noise at %.0f dBFS comes out at %.1f\n", db, out);
        CHECK(out < db - 15.0);
    }
    // Reset starts it afresh.
    auto d = denoise::makeRnNoise();
    std::vector<float> x(4800, 0.0f);
    addNoise(x, -40, 23);
    std::vector<float> a = denoiseAll(*d, x);
    d->reset();
    std::vector<float> b = denoiseAll(*d, x);
    CHECK(a == b);
}

LevelReport clip(Chain& chain, const std::vector<float>& x)
{
    std::vector<float> out;
    chain.startClip();
    for (size_t pos = 0; pos < x.size(); pos += 960)
        chain.process(x.data() + pos, std::min<size_t>(960, x.size() - pos), out);
    chain.finish(out);
    return chain.levelReport();
}

void testChain()
{
    // A voice 20 dB over a fan: with Noise in, the quiet stretches get much
    // quieter while the voice keeps its level.
    std::vector<float> x = speech(48000, 12.0, 24);
    setActiveLevel(x, 48000, -30);
    addFanNoise(x, -50, 24);

    Chain plain(48000);
    Chain cleaned(48000, denoise::makeRnNoise());
    CHECK(cleaned.hasDenoiser());
    ChainSettings full;
    full.noiseAmount = 1.0;
    cleaned.setSettings(full);
    clip(plain, x);
    clip(cleaned, x);
    LevelReport p = clip(plain, x);
    LevelReport c = clip(cleaned, x);
    std::fprintf(stderr, "chain: without noise reduction active %.1f noise %.1f margin %.1f; "
                         "with it active %.1f noise %.1f margin %.1f\n",
                 p.activeDb, p.noiseDb, p.marginDb, c.activeDb, c.noiseDb, c.marginDb);
    CHECK(c.marginDb > p.marginDb + 10.0);
    CHECK(std::fabs(c.activeDb - LEVEL_TARGET_DBFS) <= 3.0);
    CHECK(cleaned.takeReadings().voiceProbability >= 0.0f);

    // The default amount (70%) lands between.
    Chain partway(48000, denoise::makeRnNoise());
    clip(partway, x);
    LevelReport m = clip(partway, x);
    CHECK(m.marginDb > p.marginDb + 5.0 && m.marginDb < c.marginDb);

    // Pushed out, the Noise stage changes nothing but the delay.
    Chain off(48000, denoise::makeRnNoise());
    ChainSettings none;
    none.noiseIn = false;
    off.setSettings(none);
    clip(off, x);
    LevelReport o = clip(off, x);
    CHECK(std::fabs(o.marginDb - p.marginDb) < 1.0);
    CHECK(std::fabs(o.activeDb - p.activeDb) < 0.5);
    CHECK(off.latencySeconds() > plain.latencySeconds());
}

} // namespace

int main()
{
    if (!denoise::haveRnNoise())
    {
        CHECK(denoise::makeRnNoise() == nullptr);
        std::printf("built without RNNoise\n");
        return ragtime_test::finish();
    }
    auto d = denoise::makeRnNoise();
    CHECK(d != nullptr);
    if (!d)
        return ragtime_test::finish();
    CHECK_EQ(d->latency(), Denoiser::FRAME);
    std::printf("%s\n", d->name());
    testDelay();
    testNoise();
    testChain();
    return ragtime_test::finish();
}
