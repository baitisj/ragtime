// ragtime_voice_chain: runs a WAV recording through the voice chain and
// writes what sotto would get (8 kHz, 16-bit mono), printing the Level
// check. For trying the chain on real voices with no deck and no window.
//
//   ragtime_voice_chain IN.wav OUT.wav [--gain DB] [--hp 80|100|150|off]
//                       [--noise PCT|off] [--target DBFS] [--peaks]
//
// IN.wav: PCM 16-bit or 32-bit float, any rate, any channels (mixed down).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ragtime/denoise/RnNoise.h"
#include "ragtime/voice/Chain.h"

using namespace ragtime;
using namespace ragtime::voice;

namespace
{

uint32_t u32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint16_t u16(const unsigned char* p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

bool readWav(const std::string& path, std::vector<float>& out, int& rate, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a WAV file";
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    rate = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size())
    {
        uint32_t size = u32(d.data() + pos + 4);
        const unsigned char* body = d.data() + pos + 8;
        size_t avail = std::min<size_t>(size, d.size() - pos - 8);
        if (std::memcmp(d.data() + pos, "fmt ", 4) == 0 && avail >= 16)
        {
            format = u16(body);
            channels = u16(body + 2);
            rate = (int)u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26)
                format = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE's sub-format
        }
        else if (std::memcmp(d.data() + pos, "data", 4) == 0)
        {
            if (channels <= 0 || rate <= 0)
                break;
            bool pcm16 = format == 1 && bits == 16, float32 = format == 3 && bits == 32;
            if (!pcm16 && !float32)
            {
                error = "only 16-bit PCM and 32-bit float WAV files";
                return false;
            }
            size_t frame = (size_t)channels * (size_t)(bits / 8);
            size_t frames = avail / frame;
            out.resize(frames);
            for (size_t i = 0; i < frames; i++)
            {
                double sum = 0;
                for (int c = 0; c < channels; c++)
                {
                    const unsigned char* s = body + i * frame + (size_t)c * (size_t)(bits / 8);
                    if (pcm16)
                        sum += (int16_t)u16(s) / 32768.0;
                    else
                    {
                        uint32_t v = u32(s);
                        float x;
                        std::memcpy(&x, &v, 4);
                        sum += x;
                    }
                }
                out[i] = (float)(sum / channels);
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    error = "no audio found";
    return false;
}

void put32(std::ofstream& f, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    f.write((const char*)b, 4);
}

void put16(std::ofstream& f, uint16_t v)
{
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    f.write((const char*)b, 2);
}

bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    std::ofstream f(path, std::ios::binary);
    uint32_t bytes = (uint32_t)(pcm.size() * 2);
    f.write("RIFF", 4);
    put32(f, 36 + bytes);
    f.write("WAVEfmt ", 8);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)rate * 2);
    put16(f, 2);
    put16(f, 16);
    f.write("data", 4);
    put32(f, bytes);
    for (int16_t s : pcm)
        put16(f, (uint16_t)s);
    return (bool)f;
}

int usage()
{
    std::fprintf(stderr, "usage: ragtime_voice_chain IN.wav OUT.wav [--gain DB] [--hp 80|100|150|off] "
                         "[--noise PCT|off] [--target DBFS] [--peaks]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> files;
    ChainSettings s;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--gain")
            s.inputGainDb = std::atof(value().c_str());
        else if (a == "--hp")
        {
            std::string v = value();
            s.highPassIn = v != "off";
            if (s.highPassIn)
                s.highPassHz = std::atof(v.c_str());
        }
        else if (a == "--noise")
        {
            std::string v = value();
            s.noiseIn = v != "off";
            if (s.noiseIn)
                s.noiseAmount = std::atof(v.c_str()) / 100.0;
        }
        else if (a == "--target")
            s.targetDb = std::atof(value().c_str());
        else if (a == "--peaks")
            s.peakMode = true;
        else if (!a.empty() && a[0] == '-')
            return usage();
        else
            files.push_back(a);
    }
    if (files.size() != 2)
        return usage();

    std::vector<float> in;
    int rate = 0;
    std::string error;
    if (!readWav(files[0], in, rate, error))
    {
        std::fprintf(stderr, "%s: %s\n", files[0].c_str(), error.c_str());
        return 1;
    }

    Chain chain(rate, denoise::makeRnNoise());
    chain.setSettings(s);
    s = chain.settings();
    std::printf("in: %s, %d Hz, %.2f s\n", files[0].c_str(), rate, (double)in.size() / rate);
    std::printf("noise reducer: %s\n", chain.hasDenoiser() ? chain.denoiser()->name() : "none");

    // As a deck would: 10 ms at a time, one clip.
    std::vector<float> out;
    size_t piece = (size_t)std::max(1, rate / 100);
    double lowestGain = 1e9;
    chain.startClip();
    for (size_t pos = 0; pos < in.size(); pos += piece)
    {
        chain.process(in.data() + pos, std::min(piece, in.size() - pos), out);
        lowestGain = std::min(lowestGain, chain.takeReadings().levellerGainDb);
    }
    chain.finish(out);
    Readings r = chain.takeReadings();
    LevelReport c = chain.levelReport();

    std::vector<int16_t> pcm(out.size());
    toPcm16(out.data(), out.size(), pcm.data());
    if (!writeWav(files[1], pcm, Chain::OUT_RATE))
    {
        std::fprintf(stderr, "%s: can't write\n", files[1].c_str());
        return 1;
    }

    std::printf("settings: gain %+.1f dB, high-pass %s, noise %s, target %.1f dBFS, peaks %s\n", s.inputGainDb,
                s.highPassIn ? (std::to_string((int)s.highPassHz) + " Hz").c_str() : "off",
                s.noiseIn ? (std::to_string((int)std::lround(s.noiseAmount * 100)) + "%").c_str() : "off",
                s.targetDb, s.peakMode ? "out" : "in");
    std::printf("input: active %.1f dBFS (live)\n", r.inputActiveDb);
    std::printf("leveller: gain %+.1f dB at the end, lowest %+.1f dB\n", r.levellerGainDb, lowestGain);
    std::printf("out: %s, 8000 Hz, %.2f s (latency %.1f ms)\n", files[1].c_str(), c.seconds,
                chain.latencySeconds() * 1000);
    std::printf("level check:\n");
    std::printf("  active   %-5s %.1f dBFS (P.56, %.0f%% speech; target %.1f)\n", lampName(c.active), c.activeDb,
                c.activity * 100, s.targetDb);
    std::printf("  loudest  %-5s %.1f dBFS 10 ms (before the limiter %.1f; ceiling %.1f; %llu clipped)\n",
                lampName(c.loudest), c.loudestDb, c.loudestBeforeLimiterDb, LEVEL_MAX_DBFS,
                (unsigned long long)c.clippedSamples);
    std::printf("  noise    %-5s %.1f dBFS (%.1f dB under the voice)\n", lampName(c.noise), c.noiseDb, c.marginDb);
    return 0;
}
