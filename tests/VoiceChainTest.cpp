// The voice processing chain (core/src/voice/): each stage alone, then the
// chain end to end on synthetic voices, without a noise reducer (RNNoise's
// own tests are in DenoiseTest.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Check.h"
#include "VoiceSynth.h"
#include "ragtime/voice/Chain.h"
#include "ragtime/voice/Dsp.h"
#include "ragtime/voice/Levels.h"

using namespace ragtime::voice;
using namespace voice_synth;

namespace
{

bool near(double a, double b, double tol)
{
    if (std::fabs(a - b) > tol)
    {
        std::fprintf(stderr, "  %.3f vs %.3f (tolerance %.3f)\n", a, b, tol);
        return false;
    }
    return true;
}

// The gain a high-pass at 48 kHz gives a sine, in dB (after it settles).
double highPassGain(double corner, double freq)
{
    HighPass hp(48000, corner);
    std::vector<float> x = sine(48000, freq, 2.0, -20);
    hp.process(x.data(), x.size());
    return rmsDb(x, x.size() / 2) + 20.0;
}

void testHighPass()
{
    CHECK(near(highPassGain(100, 100), -3.01, 0.1));
    CHECK(highPassGain(100, 50) < -23.0);
    CHECK(highPassGain(100, 60) < -17.0);
    CHECK(near(highPassGain(100, 1000), 0.0, 0.05));
    CHECK(near(highPassGain(150, 150), -3.01, 0.1));
    CHECK(near(highPassGain(80, 300), 0.0, 0.1));
    // Changing the corner keeps going without a jump.
    HighPass hp(48000, 100);
    std::vector<float> x = sine(48000, 1000, 0.2, -20);
    hp.process(x.data(), x.size() / 2);
    hp.setCorner(150);
    hp.process(x.data() + x.size() / 2, x.size() - x.size() / 2);
    float biggest = 0;
    for (size_t i = 4800; i < x.size(); i++)  // after the first 0.1 s
        biggest = std::max(biggest, std::fabs(x[i]));
    CHECK(biggest < 0.16f);
}

double resampledGain(int in, int out, double freq)
{
    Resampler r(in, out);
    std::vector<float> x = sine(in, freq, 1.0, -20), y;
    // In odd pieces, as audio comes.
    size_t pos = 0, piece = 37;
    while (pos < x.size())
    {
        size_t n = std::min(piece, x.size() - pos);
        r.process(x.data() + pos, n, y);
        pos += n;
        piece = piece * 7 % 1013 + 1;
    }
    CHECK(near((double)y.size(), (double)x.size() * out / in, 1.0));
    return rmsDb(y, y.size() / 4, y.size() * 3 / 4) + 20.0;
}

void testResampler()
{
    CHECK(near(resampledGain(48000, 8000, 1000), 0.0, 0.05));
    CHECK(near(resampledGain(48000, 8000, 3500), 0.0, 0.1));
    // 5 kHz would fold to 3 kHz: it must be gone.
    CHECK(resampledGain(48000, 8000, 5000) < -75.0);
    CHECK(resampledGain(48000, 8000, 4400) < -75.0);
    CHECK(near(resampledGain(44100, 48000, 1000), 0.0, 0.05));
    CHECK(near(resampledGain(44100, 48000, 18000), 0.0, 0.1));
    CHECK(near(resampledGain(16000, 48000, 3000), 0.0, 0.05));
    CHECK(near(resampledGain(8000, 48000, 440), 0.0, 0.05));

    // The delay it reports lines a click up.
    Resampler r(48000, 8000);
    std::vector<float> x(48000, 0.0f), y;
    x[6000] = 1.0f;
    r.process(x.data(), x.size(), y);
    size_t peak = (size_t)(std::max_element(y.begin(), y.end(), [](float a, float b) {
                               return std::fabs(a) < std::fabs(b);
                           }) - y.begin());
    CHECK(near((double)peak, (6000.0 + (double)r.delayIn()) / 6.0, 1.0));
}

void testActiveLevel()
{
    // A steady sine is active throughout: its level is its RMS.
    {
        ActiveLevel a(8000);
        std::vector<float> x = sine(8000, 440, 3.0, -20);
        a.add(x.data(), x.size());
        CHECK(near(a.levelDb(), -20.0, 0.2));
        CHECK(a.activity() > 0.99);
    }
    // On for 1 s, off for 1 s: the pauses don't count (but for P.56's
    // 200 ms hangover after each burst, worth 0.8 dB).
    {
        ActiveLevel a(8000);
        std::vector<float> x;
        for (int k = 0; k < 4; k++)
        {
            std::vector<float> on = sine(8000, 440, 1.0, -30);
            x.insert(x.end(), on.begin(), on.end());
            x.insert(x.end(), 8000, 0.0f);
        }
        a.add(x.data(), x.size());
        CHECK(near(a.longTermDb(), -33.0, 0.1));
        CHECK(near(a.levelDb(), -30.8, 0.4));
        CHECK(near(a.activity(), 0.6, 0.05));
    }
    // Silence is silence.
    {
        ActiveLevel a(8000);
        std::vector<float> x(8000, 0.0f);
        a.add(x.data(), x.size());
        CHECK(a.levelDb() <= LEVEL_FLOOR_DBFS);
        CHECK(a.activity() == 0.0);
    }
    // Speech in a little noise: the active level ignores the pauses.
    {
        std::vector<float> x = speech(8000, 8.0, 1);
        setActiveLevel(x, 8000, -26);
        ActiveLevel a(8000);
        a.add(x.data(), x.size());
        CHECK(near(a.levelDb(), -26.0, 0.01));
        CHECK(a.longTermDb() < a.levelDb() - 0.5);  // the pauses pull the plain RMS down
        CHECK(a.activity() > 0.4 && a.activity() < 0.95);
    }
}

void testLiveLevel()
{
    // On speech it agrees with P.56 within a dB or so, and holds through a
    // long pause.
    for (uint64_t seed = 1; seed <= 4; seed++)
    {
        std::vector<float> x = speech(48000, 10.0, seed);
        setActiveLevel(x, 48000, -30);
        addNoise(x, -75, seed);
        LiveLevel live(2.0);
        for (size_t i = 0; i + 480 <= x.size(); i += 480)
        {
            double s = 0;
            for (size_t k = 0; k < 480; k++)
                s += (double)x[i + k] * x[i + k];
            live.addBlock(s / 480);
        }
        CHECK(near(live.levelDb(), -30.0, 1.5));
        // The 200 ms hangover counts, as in P.56; after it, nothing moves.
        for (int k = 0; k < 25; k++)
            live.addBlock(1e-8);  // -80 dBFS
        double before = live.levelDb();
        for (int k = 0; k < 300; k++)
            live.addBlock(1e-8);
        CHECK(near(live.levelDb(), before, 0.0001));
    }
}

void testLimiter()
{
    const int rate = 8000;
    // Below its ceilings it leaves no trace (just the delay).
    {
        Limiter lim(rate);
        std::vector<float> x = speech(rate, 4.0, 3), y;
        setActiveLevel(x, rate, -26);
        lim.process(x.data(), x.size(), y);
        CHECK_EQ(y.size(), x.size() - lim.delay());
        bool same = true;
        for (size_t i = 0; i < y.size(); i++)
            same = same && y[i] == x[i];
        CHECK(same);
        CHECK(lim.takeReductionDb() == 0.0);
    }
    // Far too loud: every 10 ms under its RMS ceiling and every sample
    // under its peak ceiling.
    {
        Limiter lim(rate);
        std::vector<float> x = speech(rate, 4.0, 4), y;
        setActiveLevel(x, rate, -4);
        std::vector<float> loud = sine(rate, 300, 0.5, 0.0);  // a full-scale tone burst
        std::copy(loud.begin(), loud.end(), x.begin() + 8000);
        lim.process(x.data(), x.size(), y);
        double worstRms = -200, worstPeak = 0;
        for (size_t i = 0; i + 80 <= y.size(); i++)
            worstRms = std::max(worstRms, rmsDb(y.data() + i, 80));
        for (float s : y)
            worstPeak = std::max(worstPeak, (double)std::fabs(s));
        CHECK(worstRms <= LEVEL_MAX_DBFS - 1.0 + 0.01);
        CHECK(20 * std::log10(worstPeak) <= -1.0 + 0.01);
        CHECK(lim.takeReductionDb() > 6.0);
    }
    // After a burst the gain comes back (50 ms time constant).
    {
        Limiter lim(rate);
        std::vector<float> x = sine(rate, 500, 1.0, -20), y;
        std::vector<float> burst = sine(rate, 500, 0.1, -3);
        std::copy(burst.begin(), burst.end(), x.begin() + 2000);
        lim.process(x.data(), x.size(), y);
        // Half a second after the burst it is back to -20 dBFS.
        size_t at = 2000 + 800 + 4000;
        CHECK(near(rmsDb(y, at - lim.delay(), at - lim.delay() + 800), -20.0, 0.05));
    }
}

// Runs blocks of 10 ms at 48 kHz through a leveller; returns what came out.
std::vector<float> level(Leveller& lev, std::vector<float> x)
{
    for (size_t i = 0; i + 480 <= x.size(); i += 480)
        lev.processBlock(x.data() + i);
    x.resize(x.size() / 480 * 480);
    return x;
}

void testLeveller()
{
    // Quiet and loud talkers both come out near the target once it has
    // heard a few seconds.
    for (double in : {-46.0, -36.0, -26.0, -14.0})
    {
        Leveller lev(480);
        std::vector<float> x = speech(48000, 16.0, 7);
        setActiveLevel(x, 48000, in);
        addNoise(x, in - 45, 7);
        std::vector<float> y = level(lev, x);
        std::vector<float> tail(y.begin() + (long)y.size() / 2, y.end());
        ActiveLevel a(48000);
        a.add(tail.data(), tail.size());
        std::fprintf(stderr, "leveller: in %.0f -> out %.2f dBFS, gain %.2f dB\n", in, a.levelDb(),
                     lev.gainDb());
        CHECK(near(a.levelDb(), LEVEL_TARGET_DBFS, 1.5));
    }
    // Another target.
    {
        Leveller lev(480);
        lev.setTarget(-20);
        std::vector<float> x = speech(48000, 16.0, 8);
        setActiveLevel(x, 48000, -35);
        std::vector<float> y = level(lev, x);
        std::vector<float> tail(y.begin() + (long)y.size() / 2, y.end());
        ActiveLevel a(48000);
        a.add(tail.data(), tail.size());
        CHECK(near(a.levelDb(), -20.0, 1.5));
    }
    // The gain holds still in a pause, however long.
    {
        Leveller lev(480);
        std::vector<float> x = speech(48000, 6.0, 9);
        setActiveLevel(x, 48000, -40);
        level(lev, x);
        double gain = lev.gainDb();
        std::vector<float> quiet(48000 * 5, 0.0f);
        addNoise(quiet, -80, 9);
        level(lev, quiet);
        CHECK(near(lev.gainDb(), gain, 0.001));
        CHECK(!lev.speaking());
    }
    // It moves slowly once settled: a talker 10 dB louder takes seconds.
    {
        Leveller lev(480);
        std::vector<float> x = speech(48000, 8.0, 10);
        setActiveLevel(x, 48000, -30);
        level(lev, x);
        double settled = lev.gainDb();
        std::vector<float> loud = speech(48000, 1.0, 11);
        setActiveLevel(loud, 48000, -20);
        level(lev, loud);
        double drop = settled - lev.gainDb();
        CHECK(drop > 0.5 && drop <= Leveller::DOWN_DB_PER_S * 1.0 + 0.01);
    }
}

// The loudest 10 ms block (of 480 samples) from `from` on.
double loudestBlock(const std::vector<float>& y, size_t from)
{
    double worst = LEVEL_FLOOR_DBFS;
    for (size_t i = from / 480 * 480; i + 480 <= y.size(); i += 480)
        worst = std::max(worst, rmsDb(y.data() + i, 480));
    return worst;
}

void testPeakMode()
{
    // A talker at a normal level who shouts now and then.
    std::vector<float> x = speech(48000, 20.0, 12);
    setActiveLevel(x, 48000, -26);
    for (int k = 0; k < 3; k++)
    {
        size_t at = (size_t)(48000 * (8.0 + 2.0 * k));
        for (size_t i = at; i < at + 48000 * 3 / 10 && i < x.size(); i++)
            x[i] *= 6.0f;  // +16 dB
    }
    Leveller pushed(480), popped(480);
    popped.setPeakMode(true);
    // Both settle on the same first 8 s, before any shout. Stop half a
    // second after the last.
    x.resize((size_t)(48000 * 12.8));
    std::vector<float> a = level(pushed, x);
    std::vector<float> b = level(popped, x);
    double loudPushed = loudestBlock(a, 48000 * 8);
    double loudPopped = loudestBlock(b, 48000 * 8);
    std::fprintf(stderr, "peak mode: loudest 10 ms pushed in %.2f, popped out %.2f dBFS; gain %.2f vs %.2f\n",
                 loudPushed, loudPopped, pushed.gainDb(), popped.gainDb());
    // Pushed in, the shouts go through to the limiter.
    CHECK(loudPushed > PEAK_MODE_DBFS + 1.0);
    // Popped out, no 10 ms block comes out over the mark...
    CHECK(loudPopped <= PEAK_MODE_DBFS + 0.5);
    // ...and the voice stays turned down after the shouting.
    CHECK(popped.gainDb() < pushed.gainDb() - 2.0);
    CHECK(popped.peakCutDb() > 2.0);
    // Pushed back in, the cut goes.
    popped.setPeakMode(false);
    std::vector<float> more = speech(48000, 0.5, 19);
    setActiveLevel(more, 48000, -26);
    level(popped, more);
    CHECK(popped.peakCutDb() == 0.0);
}

void testLevelCheck()
{
    // Speech at the target with little noise: three greens.
    {
        std::vector<float> x = speech(8000, 8.0, 13);
        setActiveLevel(x, 8000, -26);
        addNoise(x, -70, 13);
        LevelCheck check;
        check.add(x.data(), x.data(), x.size());
        LevelReport r = check.report();
        std::fprintf(stderr, "level check: active %.1f, loudest %.1f, noise %.1f, margin %.1f\n", r.activeDb,
                     r.loudestDb, r.noiseDb, r.marginDb);
        CHECK(r.speech);
        CHECK(near(r.activeDb, -26.0, 0.3));
        CHECK(near(r.noiseDb, -70.0, 2.0));
        CHECK(r.loudestDb > -20 && r.loudestDb < LEVEL_MAX_DBFS);
        CHECK(r.active == Lamp::Green);
        CHECK(r.loudest == Lamp::Green);
        CHECK(r.noise == Lamp::Green);
        CHECK(near(r.seconds, 8.0, 0.001));
    }
    // Too quiet, noisy: active and noise lamps say so.
    {
        std::vector<float> x = speech(8000, 8.0, 14);
        setActiveLevel(x, 8000, -36);
        addNoise(x, -50, 14);
        LevelCheck check;
        check.add(x.data(), x.data(), x.size());
        LevelReport r = check.report();
        CHECK(r.active == Lamp::Red);
        CHECK(r.noise == Lamp::Red);
    }
    // The microphone clipped: Loudest is red whatever the limiter did.
    {
        std::vector<float> x = speech(8000, 2.0, 15);
        setActiveLevel(x, 8000, -26);
        LevelCheck check;
        check.add(x.data(), x.data(), x.size());
        check.addClipped(3);
        CHECK(check.report().loudest == Lamp::Red);
    }
    // Silence: no speech, red lamps, no crash.
    {
        std::vector<float> x(16000, 0.0f);
        LevelCheck check;
        check.add(x.data(), x.data(), x.size());
        LevelReport r = check.report();
        CHECK(!r.speech);
        CHECK(r.active == Lamp::Red);
    }
    // The lamps' edges.
    CHECK(activeLamp(-28.9, -26, true) == Lamp::Green);
    CHECK(activeLamp(-31.0, -26, true) == Lamp::Amber);
    CHECK(activeLamp(-20.5, -26, true) == Lamp::Amber);
    CHECK(activeLamp(-33.0, -26, true) == Lamp::Red);
    CHECK(loudestLamp(-7.4, 0) == Lamp::Green);
    CHECK(loudestLamp(-6.0, 0) == Lamp::Amber);
    CHECK(loudestLamp(-4.0, 0) == Lamp::Red);
    CHECK(noiseLamp(31, true) == Lamp::Green);
    CHECK(noiseLamp(25, true) == Lamp::Amber);
    CHECK(noiseLamp(15, true) == Lamp::Red);
}

// Feeds x through the chain in pieces of `piece` samples, as a clip.
std::vector<float> runClip(Chain& chain, const std::vector<float>& x, size_t piece)
{
    std::vector<float> out;
    chain.startClip();
    for (size_t pos = 0; pos < x.size(); pos += piece)
        chain.process(x.data() + pos, std::min(piece, x.size() - pos), out);
    chain.finish(out);
    return out;
}

void testChain()
{
    // A quiet talker at 48 kHz with some hum: out at 8 kHz, near the
    // target, three green lamps, and the length the clip should have.
    {
        Chain chain(48000);
        std::vector<float> x = speech(48000, 12.0, 16);
        setActiveLevel(x, 48000, -38);
        std::vector<float> hum = sine(48000, 60, 12.0, -55);
        for (size_t i = 0; i < x.size(); i++)
            x[i] += hum[i];
        addNoise(x, -85, 16);
        // A first clip lets the leveller find the voice.
        runClip(chain, x, 480);
        std::vector<float> out = runClip(chain, x, 441);
        LevelReport r = chain.levelReport();
        std::fprintf(stderr, "chain: active %.2f, loudest %.2f (before limiter %.2f), noise %.1f, %.3f s, "
                             "gain %.2f dB, latency %.1f ms\n",
                     r.activeDb, r.loudestDb, r.loudestBeforeLimiterDb, r.noiseDb, r.seconds,
                     chain.takeReadings().levellerGainDb, chain.latencySeconds() * 1000);
        // The clip: the microphone's 12 s at 8 kHz, plus at most the
        // chain's delay and 20 ms of what came before it.
        double least = 12.0 * 8000;
        double most = least + chain.latencySeconds() * 8000 + 160;
        CHECK((double)out.size() >= least && (double)out.size() <= most);
        CHECK(near(r.seconds, (double)out.size() / 8000, 1e-9));
        CHECK(r.active == Lamp::Green);
        CHECK(r.loudest == Lamp::Green);
        CHECK(r.noise == Lamp::Green);
        // The hum went with the high-pass: 60 Hz at -55 dBFS, boosted 12 dB
        // by the leveller, would be at -43 dBFS.
        CHECK(r.noiseDb < -58);
        // Pushed out, it stays.
        ChainSettings off;
        off.highPassIn = false;
        chain.setSettings(off);
        runClip(chain, x, 480);
        std::fprintf(stderr, "chain: high-pass out, noise %.1f dBFS\n", chain.levelReport().noiseDb);
        CHECK(chain.levelReport().noiseDb > -46);
        CHECK(chain.levelReport().noise == Lamp::Amber || chain.levelReport().noise == Lamp::Red);
    }
    // 44.1 kHz in, Gain knob up, too loud: the limiter holds sotto's ceiling.
    {
        Chain chain(44100);
        ChainSettings s;
        s.inputGainDb = 20;
        s.peakMode = false;
        chain.setSettings(s);
        std::vector<float> x = speech(44100, 6.0, 17);
        setActiveLevel(x, 44100, -20);  // +20 dB gain: -0 dBFS active
        for (float& v : x)
            v = std::clamp(v, -0.99f, 0.99f);
        std::vector<float> out = runClip(chain, x, 1024);
        double worst = LEVEL_FLOOR_DBFS;
        for (size_t i = 0; i + 80 <= out.size(); i += 40)
            worst = std::max(worst, rmsDb(out.data() + i, 80));
        CHECK(worst <= LEVEL_MAX_DBFS);
        Readings rd = chain.takeReadings();
        CHECK(rd.levellerGainDb < -10);
        // Read once, the burst maxima start again.
        Readings again = chain.takeReadings();
        CHECK(again.inputLoudestDb <= LEVEL_FLOOR_DBFS);
        CHECK(!again.clipped);
    }
    // Clipping at the microphone shows on the meters and the check.
    {
        Chain chain(48000);
        std::vector<float> x = speech(48000, 3.0, 18);
        setActiveLevel(x, 48000, -10);
        for (float& v : x)
            v = std::clamp(v * 4.0f, -1.0f, 1.0f);
        runClip(chain, x, 960);
        CHECK(chain.takeReadings().clipped);
        CHECK(chain.levelReport().clippedSamples > 0);
        CHECK(chain.levelReport().loudest == Lamp::Red);
    }
    // Settings clamp to the knobs' ranges.
    {
        ChainSettings s;
        s.inputGainDb = 99;
        s.highPassHz = 120;
        s.noiseAmount = 2;
        s.targetDb = -50;
        ChainSettings c = s.clamped();
        CHECK(c.inputGainDb == 40);
        CHECK(c.highPassHz == 100);
        CHECK(c.noiseAmount == 1);
        CHECK(c.targetDb == -36);
    }
    // 16-bit samples, as sotto takes them.
    {
        float f[4] = {0.5f, -1.0f, 1.5f, 1.0f / 32768};
        int16_t p[4];
        toPcm16(f, 4, p);
        CHECK_EQ(p[0], 16384);
        CHECK_EQ(p[1], -32767);
        CHECK_EQ(p[2], 32767);
        CHECK_EQ(p[3], 1);
        float back[4];
        fromPcm16(p, 4, back);
        CHECK(back[0] == 0.5f);
    }
}

} // namespace

int main()
{
    testHighPass();
    testResampler();
    testActiveLevel();
    testLiveLevel();
    testLimiter();
    testLeveller();
    testPeakMode();
    testLevelCheck();
    testChain();
    return ragtime_test::finish();
}
