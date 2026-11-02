// voice::Sotto, libsotto loaded at run time. With no libsotto to be found
// (CI has none) only the refusals are checked; with one (RAGTIME_SOTTO, or
// -DRAGTIME_SOTTO_TEST_LIB=... at configure time) a clip goes through the
// 16 kHz encoder and decoder (codec 2) and comes back at about its level,
// and a codec 1 clip plays through the 8 kHz decoder, doubled.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/voice/ArrivalPlayer.h"
#include "ragtime/voice/Dsp.h"
#include "ragtime/voice/Sotto.h"
#include "ragtime/voice/Voice.h"

using namespace ragtime;

// M_PI is POSIX, not C++: MinGW has none without _USE_MATH_DEFINES.
constexpr double PI = 3.14159265358979323846;

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
        double f0 = 120.0 + 15.0 * std::sin(2 * PI * 1.5 * t);
        phase += 2 * PI * f0 / rate;
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
    const size_t block = voice::SOTTO_SAMPLES;
    std::vector<double> blocks;
    for (size_t i = 0; i + block <= x.size(); i += block)
    {
        double sum = 0.0;
        for (size_t j = 0; j < block; j++) sum += (double)x[i + j] * x[i + j];
        blocks.push_back(sum / (double)block);
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

// x's power above 4 kHz against below, in dB: the band below is x brought
// down to 8 kHz.
double highBandDb(const std::vector<int16_t>& x)
{
    std::vector<float> in = voice::toFloat(x), low;
    voice::Resampler down(voice::SOTTO_RATE, 8000);
    down.process(in.data(), in.size(), low);
    double all = 0.0, below = 0.0;
    for (float v : in) all += (double)v * v;
    for (float v : low) below += (double)v * v;
    all /= std::max<size_t>(1, in.size());
    below /= std::max<size_t>(1, low.size());
    return 10.0 * std::log10(std::max(1e-12, all - below) / std::max(1e-12, below));
}

// The test clip from sotto v3.6's encoder: codec 1's frames.
std::vector<voice::SottoFrame> codec1Clip()
{
    std::ifstream in(std::string(RAGTIME_SOURCE_DIR) + "/tests/data/voice/libri198-v36.bit", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<voice::SottoFrame> frames;
    for (size_t i = 0; i + voice::SOTTO_FRAME_BYTES <= bytes.size(); i += voice::SOTTO_FRAME_BYTES)
    {
        voice::SottoFrame f;
        std::copy(bytes.begin() + (std::ptrdiff_t)i, bytes.begin() + (std::ptrdiff_t)i + voice::SOTTO_FRAME_BYTES, f.begin());
        frames.push_back(f);
    }
    return frames;
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
        CHECK(!none.decoder(voice::CODEC_SOTTO_V38));
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
    CHECK_EQ((int)lib.info().codec, (int)voice::CODEC_SOTTO_V38);
    CHECK(!lib.decoder(0));
    CHECK(!lib.decoder(3));

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
    enc->push(in.data(), voice::SOTTO_SAMPLES, frames);
    CHECK_EQ(frames.size(), before);

    // The silence either side is silence; the buzz is voiced near 120 Hz.
    CHECK(voice::isSilent(frames.front()));
    CHECK(voice::isSilent(frames.back()));
    const voice::Frame& mid = frames[frames.size() / 2];
    CHECK(!voice::isSilent(mid));
    std::printf("middle frame: %.0f Hz\n", voice::pitchHzOf(mid));
    CHECK(voice::pitchHzOf(mid) > 90.0 && voice::pitchHzOf(mid) < 160.0);

    // The decoder gives the level back, with the delay taken out.
    auto dec = lib.decoder(voice::CODEC_SOTTO_V38);
    CHECK(dec != nullptr);
    CHECK_EQ(dec->delay(), 5);
    std::vector<int16_t> out = dec->decodeClip(frames, {});
    CHECK_EQ(out.size(), frames.size() * voice::SOTTO_SAMPLES);
    double inDb = activeDbfs(in), outDb = activeDbfs(out);
    std::printf("active level in %.1f dBFS, out %.1f dBFS\n", inDb, outDb);
    CHECK(std::fabs(inDb - outDb) < 4.0);
    // The start stays quiet: no 20 ms shift of the buzz into the lead-in.
    double lead = 0.0;
    for (size_t i = 0; i < (size_t)voice::SOTTO_RATE / 10; i++) lead = std::max(lead, std::fabs((double)out[i]));
    CHECK(lead < 200.0);

    // Lost frames are covered and then go quiet.
    std::vector<uint8_t> good(frames.size(), 1);
    for (size_t i = 40; i < 60; i++) good[i] = 0;
    std::vector<int16_t> holed = dec->decodeClip(frames, good);
    double tail = 0.0;
    for (size_t i = 52 * (size_t)voice::SOTTO_SAMPLES; i < 58 * (size_t)voice::SOTTO_SAMPLES; i++)
        tail = std::max(tail, std::fabs((double)holed[i]));
    std::printf("lost frames: loudest %.0f 240 ms in\n", tail);
    CHECK(tail < 50.0);

    // A codec 1 clip plays through the 8 kHz decoder, doubled: about its
    // level, and no loud hiss band (sotto_decode16 would read its unvoiced
    // frames' pitch as hiss).
    std::vector<voice::SottoFrame> old = codec1Clip();
    CHECK(old.size() > 600);
    auto dec1 = lib.decoder(voice::CODEC_SOTTO_V36);
    CHECK(dec1 != nullptr);
    CHECK_EQ(dec1->delay(), 1);
    std::vector<int16_t> heard = dec1->decodeClip(old, {});
    CHECK_EQ(heard.size(), old.size() * voice::SOTTO_SAMPLES);
    const double oldDb = activeDbfs(heard), oldHigh = highBandDb(heard);
    std::printf("codec 1 clip: active level %.1f dBFS, above 4 kHz %.1f dB\n", oldDb, oldHigh);
    CHECK(oldDb > -35.0 && oldDb < -15.0);
    CHECK(oldHigh < -25.0);

    // Played as it arrives, all of it in at once, the clip sounds as
    // decodeClip gives it: the player takes the decoder's five frames of
    // delay out the same way.
    {
        voice::PauseGate gate;
        gate.enabled = false;
        // Speech: the codec 1 clip's sound coded again as codec 2.
        auto enc2 = lib.encoder();
        std::vector<voice::SottoFrame> speech;
        enc2->push(heard.data(), heard.size(), speech);
        enc2->flush(speech);
        std::vector<voice::Frame> plain(speech.begin(), speech.end());
        std::vector<uint8_t> sot = voice::makeSot(plain, gate);
        voice::Clip clip;
        CHECK(voice::readSot(sot, clip) == voice::ReadResult::Ok);
        std::vector<voice::SottoFrame> expanded(clip.frames.begin(), clip.frames.end());
        std::vector<int16_t> whole = lib.decoder(voice::CODEC_SOTTO_V38)->decodeClip(expanded, {});
        std::shared_ptr<voice::SottoDecoder> live(lib.decoder(voice::CODEC_SOTTO_V38));
        voice::FrameDecoder calls;
        calls.decode = [live](const voice::Frame& f, bool ok, int16_t* o) { live->decode(f, ok, o); };
        calls.flush = [live](int16_t* o) { return live->flush(o); };
        calls.delay = live->delay();
        voice::ArrivalPlayer player(calls);
        player.arrived(sot, sot.size(), 0);
        player.ended();
        std::vector<int16_t> played;
        int16_t pcm[voice::FRAME_SAMPLES];
        for (size_t got; (got = player.pull(pcm, voice::FRAME_SAMPLES, 0)) > 0;) played.insert(played.end(), pcm, pcm + got);
        CHECK_EQ(played.size(), whole.size());
        // sotto's unvoiced sound is noise from rand(), so the two differ
        // sample by sample; frame by frame their levels line up.
        const size_t frames20 = whole.size() / voice::SOTTO_SAMPLES;
        auto frameDb = [](const std::vector<int16_t>& x, size_t k) {
            std::vector<int16_t> f(x.begin() + (std::ptrdiff_t)(k * voice::SOTTO_SAMPLES),
                                   x.begin() + (std::ptrdiff_t)((k + 1) * voice::SOTTO_SAMPLES));
            return activeDbfs(f);
        };
        // Frames more than 3 dB apart, the played sound shift frames on.
        auto apart = [&](size_t shift) {
            size_t n = 0;
            for (size_t k = 0; k + shift < frames20 && played.size() == whole.size(); k++)
            {
                const double da = frameDb(played, k + shift), db = frameDb(whole, k);
                if (std::fabs(da - db) > 3.0 && std::max(da, db) > -60.0) n++;
            }
            return n;
        };
        const size_t worse = apart(0), shifted = apart(1);
        std::printf("played as it arrives: %zu of %zu frames more than 3 dB from decodeClip's (%zu a frame off)\n",
                    worse, frames20, shifted);
        CHECK(worse * 20 <= frames20);
        CHECK(shifted > worse * 4);
        CHECK_EQ(player.playedFrames(), (uint32_t)expanded.size());
    }


    return ragtime_test::finish();
}
