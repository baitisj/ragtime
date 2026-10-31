#include "ragtime/voice/MicCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ragtime
{
namespace voice
{

namespace
{

double dbfs(double meanSquare)
{
    // A full-scale sine is -3 dBFS RMS; a full-scale square is 0.
    return meanSquare > 1e-12 ? 10.0 * std::log10(meanSquare) : -120.0;
}

std::vector<double> frameLevels(const float* samples, std::size_t n, std::size_t frame)
{
    std::vector<double> out;
    if (frame == 0) return out;
    for (std::size_t start = 0; start + frame <= n; start += frame)
    {
        double sum = 0;
        for (std::size_t i = start; i < start + frame; i++) sum += (double)samples[i] * samples[i];
        out.push_back(dbfs(sum / (double)frame));
    }
    return out;
}

std::string number(double dB)
{
    char text[16];
    std::snprintf(text, sizeof text, "%.0f", dB);
    return text;
}

} // namespace

MicCheck checkMicrophone(const float* samples, std::size_t n, int rate)
{
    MicCheck c;
    if (!samples || rate <= 0) return c;
    for (std::size_t i = 0; i < n; i++)
        if (std::fabs(samples[i]) >= 0.999f) c.clipped = true;
    const std::vector<double> tens = frameLevels(samples, n, (std::size_t)rate / 100);
    std::vector<double> twenties = frameLevels(samples, n, (std::size_t)rate / 50);
    if (tens.empty() || twenties.empty()) return c;
    c.loudestDbfs = *std::max_element(tens.begin(), tens.end());
    std::sort(twenties.begin(), twenties.end());
    // The loudest of the quietest tenth.
    c.noiseDbfs = twenties[(twenties.size() - 1) / 10];

    using V = MicCheck::Verdict;
    if (c.loudestDbfs <= -100.0)
        c.verdict = V::Silent;
    else if (c.clipped || c.loudestDbfs > SOTTO_CEILING_DBFS)
        c.verdict = V::TooLoud;
    else if (c.loudestDbfs > PEAK_HIGH_DBFS)
        c.verdict = V::Loud;
    else if (c.loudestDbfs >= PEAK_LOW_DBFS)
        c.verdict = V::Good;
    else if (c.loudestDbfs >= TOO_QUIET_DBFS)
        c.verdict = V::Quiet;
    else
        c.verdict = V::TooQuiet;
    c.noisy = c.verdict != V::Silent && c.loudestDbfs - c.noiseDbfs < GOOD_NOISE_GAP_DB;
    return c;
}

std::vector<float> testChime(int rate)
{
    // C5 D5 E5 G5, a fifth of a second each, struck and dying away.
    const double notes[] = {523.25, 587.33, 659.25, 783.99};
    const double pi = 3.14159265358979323846;
    std::vector<float> out;
    if (rate <= 0) return out;
    const int n = rate / 5;
    for (double hz : notes)
        for (int i = 0; i < n; i++)
        {
            const double t = (double)i / rate;
            const double envelope = std::min(1.0, t * 200.0) * std::exp(-t * 6.0);
            out.push_back((float)(0.316 * envelope * std::sin(2 * pi * hz * t)));
        }
    return out;
}

std::string MicCheck::summary() const
{
    using V = MicCheck::Verdict;
    if (verdict == V::Silent) return "Nothing came from the microphone: is it muted?";
    std::string text = "Loudest " + number(loudestDbfs) + " dBFS, room " + number(noiseDbfs) + " dBFS: ";
    switch (verdict)
    {
    case V::TooQuiet:
        text += "very quiet; turn the microphone up, or speak closer";
        break;
    case V::Quiet:
        text += "a little quiet; the deck's gain will make it up";
        break;
    case V::Good:
        text += "good";
        break;
    case V::Loud:
        text += "a little loud; the deck's leveller will take care of it";
        break;
    case V::TooLoud:
        text += clipped ? "clipping; turn the microphone down" : "too loud for sotto; turn the microphone down";
        break;
    case V::Silent:
        break;
    }
    if (noisy) text += ", and the room is noisy";
    return text;
}

} // namespace voice
} // namespace ragtime
