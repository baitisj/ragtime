// Test voices for the voice chain's tests: a small formant synthesiser
// (a glottal pulse train through three vowel formants, with hissy
// consonants, syllables, words and pauses between them), so the tests need
// no recordings. Deterministic: its own random numbers, no <random>.
#ifndef RAGTIME_TESTS_VOICE_SYNTH_H
#define RAGTIME_TESTS_VOICE_SYNTH_H

#include <cmath>
#include <cstdint>
#include <vector>

#include "ragtime/voice/Levels.h"

namespace voice_synth
{

constexpr double PI = 3.14159265358979323846;

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniform() { return (double)(next() >> 11) / 9007199254740992.0; }
    double range(double a, double b) { return a + (b - a) * uniform(); }
    double gauss()
    {
        double u = uniform(), v = uniform();
        if (u < 1e-300)
            u = 1e-300;
        return std::sqrt(-2 * std::log(u)) * std::cos(2 * PI * v);
    }
};

struct Resonator
{
    double a1 = 0, a2 = 0, g = 1, y1 = 0, y2 = 0;
    void set(double rate, double freq, double bandwidth)
    {
        double r = std::exp(-PI * bandwidth / rate);
        a1 = -2 * r * std::cos(2 * PI * freq / rate);
        a2 = r * r;
        g = 1 + a1 + a2;  // unity at DC
    }
    double step(double x)
    {
        double y = g * x - a1 * y1 - a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

struct Voice
{
    double pitchHz = 120;     // average f0
    double wordPauseS = 0.4;  // average pause between words
};

inline void normalise(std::vector<double>& seg, double rms)
{
    double sum = 0;
    for (double v : seg)
        sum += v * v;
    if (sum <= 0)
        return;
    double g = rms / std::sqrt(sum / (double)seg.size());
    for (double& v : seg)
        v *= g;
}

// About `seconds` of speech-like sound at `rate`, roughly at -20 dBFS active (set it with setActiveLevel).
inline std::vector<float> speech(int rate, double seconds, uint64_t seed, Voice voice = Voice())
{
    static const double vowels[][3] = {
        {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010},
        {570, 840, 2410},  {300, 870, 2240},  {660, 1720, 2410},
    };
    Rng rng(seed);
    std::vector<float> out;
    out.reserve((size_t)(seconds * rate) + rate);
    Resonator f[3];
    double glottal1 = 0, glottal2 = 0, lastRad = 0;
    double phase = 0;
    double hp = 0;
    auto silence = [&](double s) {
        for (int i = 0, n = (int)(s * rate); i < n; i++)
            out.push_back(0.0f);
    };
    silence(0.3);
    while ((double)out.size() < seconds * rate)
    {
        int syllables = 1 + (int)(rng.uniform() * 3.5);
        for (int sy = 0; sy < syllables; sy++)
        {
            // A hissy consonant before about half the syllables: noise
            // through a broad resonance near 4 kHz, 14 dB under the vowel.
            double loud = rng.range(0.5, 1.0);
            if (rng.uniform() < 0.5)
            {
                Resonator hiss;
                hiss.set(rate, std::min(4000.0, 0.35 * rate), 2500);
                int n = (int)(rng.range(0.04, 0.09) * rate);
                std::vector<double> seg((size_t)n);
                for (int i = 0; i < n; i++)
                {
                    double w = rng.gauss();
                    double y = hiss.step(w - hp);
                    hp = w;
                    seg[(size_t)i] = y * std::sin(PI * i / n);
                }
                normalise(seg, 0.1 * loud * 0.2);
                for (double v : seg)
                    out.push_back((float)v);
            }
            const double* v = vowels[(int)(rng.uniform() * 6) % 6];
            for (int k = 0; k < 3; k++)
                f[k].set(rate, std::min(v[k] * rng.range(0.95, 1.05), 0.4 * rate), 60.0 + 40.0 * k);
            int n = (int)(rng.range(0.12, 0.26) * rate);
            int edge = (int)(0.025 * rate);
            double f0 = voice.pitchHz * rng.range(0.85, 1.2);
            std::vector<double> seg((size_t)n);
            for (int i = 0; i < n; i++)
            {
                double t = (double)i / n;
                double pitch = f0 * (1.05 - 0.1 * t) * (1 + 0.01 * std::sin(2 * PI * 5 * i / rate));
                phase += pitch / rate;
                double pulse = 0;
                if (phase >= 1)
                {
                    phase -= 1;
                    pulse = 1;
                }
                // Glottal low-pass (-12 dB an octave), then lips (+6).
                glottal1 = 0.98 * glottal1 + pulse;
                glottal2 = 0.98 * glottal2 + glottal1;
                double rad = glottal2 - lastRad;
                lastRad = glottal2;
                double y = rad;
                for (Resonator& r : f)
                    y = r.step(y);
                double env = 1;
                if (i < edge)
                    env = 0.5 - 0.5 * std::cos(PI * i / edge);
                else if (i > n - edge)
                    env = 0.5 - 0.5 * std::cos(PI * (n - i) / edge);
                seg[(size_t)i] = env * y;
            }
            normalise(seg, 0.1 * loud);
            for (double s : seg)
                out.push_back((float)s);
            silence(rng.range(0.03, 0.06));
        }
        silence(voice.wordPauseS * rng.range(0.6, 1.5));
    }
    out.resize((size_t)(seconds * rate));
    return out;
}

// Scales x so its P.56 active level is `db` dBFS.
inline void setActiveLevel(std::vector<float>& x, int rate, double db)
{
    ragtime::voice::ActiveLevel level(rate);
    level.add(x.data(), x.size());
    double gain = std::pow(10.0, (db - level.levelDb()) / 20.0);
    for (float& s : x)
        s = (float)(s * gain);
}

// White noise at `db` dBFS RMS, added.
inline void addNoise(std::vector<float>& x, double db, uint64_t seed)
{
    Rng rng(seed);
    double a = std::pow(10.0, db / 20.0);
    for (float& s : x)
        s += (float)(a * rng.gauss());
}

// Fan-like noise (white noise through a one-pole low-pass, falling from a
// few hundred hertz) at `db` dBFS RMS, added.
inline void addFanNoise(std::vector<float>& x, double db, uint64_t seed)
{
    Rng rng(seed);
    std::vector<double> n(x.size());
    double y = 0, sum = 0;
    for (double& v : n)
    {
        y = 0.95 * y + rng.gauss();
        v = y;
        sum += y * y;
    }
    double g = n.empty() ? 0 : std::pow(10.0, db / 20.0) / std::sqrt(sum / (double)n.size());
    for (size_t i = 0; i < x.size(); i++)
        x[i] += (float)(g * n[i]);
}

inline std::vector<float> sine(int rate, double freq, double seconds, double rmsDb)
{
    std::vector<float> x((size_t)(seconds * rate));
    double a = std::pow(10.0, rmsDb / 20.0) * std::sqrt(2.0);
    for (size_t i = 0; i < x.size(); i++)
        x[i] = (float)(a * std::sin(2 * PI * freq * (double)i / rate));
    return x;
}

inline double rmsDb(const float* x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++)
        s += (double)x[i] * x[i];
    return ragtime::voice::powerDb(n ? s / (double)n : 0);
}

inline double rmsDb(const std::vector<float>& x, size_t from = 0, size_t to = (size_t)-1)
{
    if (to > x.size())
        to = x.size();
    if (from >= to)
        return ragtime::voice::LEVEL_FLOOR_DBFS;
    return rmsDb(x.data() + from, to - from);
}

} // namespace voice_synth

#endif
