// Voice messages: sotto's frame fields, the .sot file (silence runs, the
// pause gate, reading a prefix and a file with holes, refusing bad files,
// and a fuzz run), the pitch strip and the airtime sums, on made-up frames
// and on a real clip in tests/data/voice/.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/voice/Voice.h"

using namespace ragtime;
using voice::Frame;

namespace
{

std::vector<Frame> loadClip()
{
    std::ifstream in(std::string(RAGTIME_SOURCE_DIR) + "/tests/data/voice/libri198-v36.bit", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Frame> frames(bytes.size() / voice::FRAME_BYTES);
    for (size_t i = 0; i < frames.size(); i++)
        std::copy(bytes.begin() + (std::ptrdiff_t)(i * 6), bytes.begin() + (std::ptrdiff_t)(i * 6 + 6),
                  frames[i].begin());
    return frames;
}

Frame speech(int energy, int pitch = 40, voice::Voicing v = voice::Voicing::Voiced, uint64_t shape = 0x2AAAAAAAAull)
{
    return voice::makeFrame(v, pitch, energy, shape);
}

// A silent frame as libsotto might send it: energy 0, other bits not.
Frame noisySilence()
{
    return voice::makeFrame(voice::Voicing::Unvoiced, 77, 0, 0x123456789ull);
}

void testFields()
{
    Frame f = voice::makeFrame(voice::Voicing::Mixed, 127, 31, 0x3FFFFFFFFull);
    CHECK(voice::voicingOf(f) == voice::Voicing::Mixed);
    CHECK_EQ(voice::pitchCodeOf(f), 127);
    CHECK_EQ(voice::energyCodeOf(f), 31);
    CHECK(std::fabs(voice::pitchHzOf(f) - 500.0) < 1e-9);
    CHECK(std::fabs(voice::energyDbfsOf(f) - (-7.3)) < 1e-9);
    // The bits sit where sotto puts them: voicing first, most significant
    // bit first.
    Frame g = voice::makeFrame(voice::Voicing::Onset, 0, 0, 0);
    CHECK_EQ((int)g[0], 0xC0);
    Frame e = voice::makeFrame(voice::Voicing::Unvoiced, 0, 1, 0);
    CHECK_EQ((int)e[1], 0x04);  // energy's last bit is the frame's 14th
    CHECK(std::fabs(voice::pitchHzOf(e) - 50.0) < 1e-9);
    CHECK(std::fabs(voice::energyDbfsOf(e) - (10.0 + 70.0 / 31.0 - 87.3)) < 1e-9);
    CHECK(voice::isSilent(noisySilence()));
    CHECK_EQ(voice::energyDbfsOf(noisySilence()), voice::SILENCE_DBFS);
    CHECK(voice::isSilent(voice::SILENT_FRAME));
}

void testWriteAndRead()
{
    std::vector<Frame> frames;
    for (int i = 0; i < 5; i++) frames.push_back(speech(20 + i, 30 + i));
    for (int i = 0; i < 100; i++) frames.push_back(noisySilence());
    for (int i = 0; i < 3; i++) frames.push_back(speech(25));
    frames.push_back(noisySilence());
    frames.push_back(speech(22));

    std::vector<uint8_t> file = voice::writeSot(frames);
    // 9 frames that sound, a run of 100 and a run of 1: 11 frames in the file.
    CHECK_EQ(file.size(), voice::HEADER_BYTES + 11 * 6);
    CHECK_EQ((int)file[0], (int)'S');
    CHECK_EQ((int)file[1], (int)'o');
    CHECK_EQ((int)file[2], 1);
    CHECK_EQ((int)file[3], 0);
    CHECK_EQ((int)file[6], (int)frames.size());
    CHECK(voice::isSot(file.data(), file.size()));

    voice::Clip clip;
    CHECK(voice::readSot(file, clip) == voice::ReadResult::Ok);
    CHECK(clip.whole);
    CHECK_EQ(clip.length, (uint32_t)frames.size());
    CHECK_EQ(clip.frames.size(), frames.size());
    CHECK(std::fabs(clip.seconds() - frames.size() * 0.02) < 1e-9);
    for (size_t i = 0; i < frames.size(); i++)
    {
        CHECK_EQ((int)clip.good[i], 1);
        // Speech comes back bit for bit; silence as SILENT_FRAME.
        CHECK(clip.frames[i] == (voice::isSilent(frames[i]) ? voice::SILENT_FRAME : frames[i]));
    }

    // A run past 65,536 frames splits.
    std::vector<Frame> longPause(70000, voice::SILENT_FRAME);
    longPause.insert(longPause.begin(), speech(20));
    longPause.push_back(speech(20));
    file = voice::writeSot(longPause);
    CHECK_EQ(file.size(), voice::HEADER_BYTES + 4 * 6);
    CHECK(voice::readSot(file, clip) == voice::ReadResult::Ok);
    CHECK_EQ(clip.frames.size(), longPause.size());
    CHECK(clip.frames.back() == longPause.back());

    // Too long to write; an empty clip is a header alone.
    CHECK(voice::writeSot(std::vector<Frame>(voice::MAX_CLIP_FRAMES + 1, speech(20))).empty());
    file = voice::writeSot({});
    CHECK_EQ(file.size(), voice::HEADER_BYTES);
    CHECK(voice::readSot(file, clip) == voice::ReadResult::Ok);
    CHECK(clip.whole && clip.frames.empty());
}

void testGateAndTrim()
{
    // Speech at code 25 (the clip's loud frames), pauses at code 10, 34 dB down.
    std::vector<Frame> frames;
    auto add = [&](int n, int energy) {
        for (int i = 0; i < n; i++) frames.push_back(energy ? speech(energy) : noisySilence());
    };
    add(6, 10);     // quiet lead-in: goes, but for the hang before the word
    add(20, 25);
    add(12, 10);    // a long pause: silent but for 2 frames each side
    add(20, 25);
    add(5, 10);     // a short pause: kept
    add(20, 25);
    add(3, 10);     // quiet, then silence, at the end
    add(4, 0);

    voice::PauseGate gate;
    std::vector<Frame> gated = voice::gatePauses(frames, gate);
    CHECK_EQ(gated.size(), frames.size());
    auto silentIn = [&](const std::vector<Frame>& f, size_t a, size_t b) {
        int n = 0;
        for (size_t i = a; i < b; i++) n += voice::isSilent(f[i]);
        return n;
    };
    CHECK_EQ(silentIn(gated, 0, 4), 4);
    CHECK_EQ(silentIn(gated, 4, 6), 0);
    CHECK_EQ(silentIn(gated, 6, 26), 0);
    CHECK_EQ(silentIn(gated, 26, 28), 0);
    CHECK_EQ(silentIn(gated, 28, 36), 8);
    CHECK_EQ(silentIn(gated, 36, 38), 0);
    CHECK_EQ(silentIn(gated, 58, 63), 0);
    CHECK_EQ(silentIn(gated, 83, 85), 0);
    CHECK_EQ(silentIn(gated, 85, 90), 5);

    std::vector<Frame> trimmed = voice::trimSilence(gated);
    CHECK_EQ(trimmed.size(), (size_t)(2 + 20 + 12 + 20 + 5 + 20 + 2));
    CHECK(!voice::isSilent(trimmed.front()) && !voice::isSilent(trimmed.back()));

    // Off, nothing changes; a gate set to 40 dB leaves 34 dB pauses alone.
    gate.enabled = false;
    CHECK(voice::gatePauses(frames, gate) == frames);
    gate.enabled = true;
    gate.belowDb = 40;
    CHECK_EQ(silentIn(voice::gatePauses(frames, gate), 0, frames.size()), 4);

    // makeSot does all three; all silence gives nothing.
    std::vector<uint8_t> file = voice::makeSot(frames);
    voice::Clip clip;
    CHECK(voice::readSot(file, clip) == voice::ReadResult::Ok);
    CHECK_EQ(clip.frames.size(), trimmed.size());
    CHECK(voice::makeSot(std::vector<Frame>(50, noisySilence())).empty());
    CHECK(voice::trimSilence(std::vector<Frame>(3, voice::SILENT_FRAME)).empty());
}

void testRealClip()
{
    std::vector<Frame> frames = loadClip();
    CHECK_EQ(frames.size(), (size_t)696);
    if (frames.empty()) return;
    std::vector<uint8_t> plain = voice::writeSot(frames);
    std::vector<uint8_t> file = voice::makeSot(frames);
    // Read speech with few pauses: the gate and the runs save a little.
    std::printf("real clip: %zu frames, %zu bytes plain, %zu bytes gated (%.1f%% saved)\n", frames.size(),
                plain.size(), file.size(), 100.0 * (1.0 - (double)file.size() / (double)plain.size()));
    CHECK(file.size() < plain.size() * 95 / 100);
    voice::Clip clip;
    CHECK(voice::readSot(file, clip) == voice::ReadResult::Ok);
    CHECK(clip.whole);
    // Every frame that still sounds is sotto's own, in its place.
    std::vector<Frame> expect = voice::trimSilence(voice::gatePauses(frames, voice::PauseGate()));
    CHECK(clip.frames == expect);

    voice::PitchStrip strip = voice::pitchStrip(clip, 120);
    CHECK_EQ(strip.columns.size(), (size_t)120);
    CHECK(strip.lowHz > 50 && strip.highHz <= 500 && strip.highHz >= strip.lowHz * 2 - 1e-9);
    // A woman reading: her pitch sits between 120 and 350 Hz.
    double mid = std::sqrt(strip.lowHz * strip.highHz);
    CHECK(mid > 120 && mid < 350);
    int voiced = 0, silent = 0;
    for (const voice::StripColumn& c : strip.columns)
    {
        CHECK(c.arrived && !c.lost);
        voiced += c.voiced;
        silent += c.silent;
        CHECK(strip.height(c) >= 0 && strip.height(c) <= 1);
        CHECK(strip.thickness(c) >= 0 && strip.thickness(c) <= 1);
    }
    CHECK(voiced > 60);
    CHECK(silent < 30);
}

void testPrefix()
{
    std::vector<uint8_t> file = voice::makeSot(loadClip());
    voice::Clip whole;
    CHECK(voice::readSot(file, whole) == voice::ReadResult::Ok);
    voice::Clip clip;
    CHECK(voice::readSot(file.data(), file.size(), 0, {}, clip) == voice::ReadResult::Short);
    CHECK(voice::readSot(file.data(), file.size(), 5, {}, clip) == voice::ReadResult::Short);
    size_t last = 0;
    for (uint64_t have = voice::HEADER_BYTES; have <= file.size(); have += 5)
    {
        CHECK(voice::readSot(file.data(), file.size(), have, {}, clip) == voice::ReadResult::Ok);
        // The length is known from the header; the frames grow, each as
        // the whole clip has it.
        CHECK_EQ(clip.length, whole.length);
        CHECK(clip.frames.size() >= last);
        CHECK(clip.frames.size() <= whole.frames.size());
        CHECK(std::equal(clip.frames.begin(), clip.frames.end(), whole.frames.begin()));
        CHECK(clip.whole == (have == file.size()));
        last = clip.frames.size();
    }
    // A strip of a clip half come: the left half arrived, the right not.
    CHECK(voice::readSot(file.data(), file.size(), file.size() / 2, {}, clip) == voice::ReadResult::Ok);
    voice::PitchStrip strip = voice::pitchStrip(clip, 10);
    CHECK(strip.columns[0].arrived && !strip.columns[9].arrived);
}

void testHoles()
{
    // Speech, a pause of 50, speech, a pause of 30, speech: 1 + 1 + 1 + 1 +
    // 1 frames in the file, each speech part 10 frames.
    std::vector<Frame> frames;
    for (int part = 0; part < 3; part++)
    {
        for (int i = 0; i < 10; i++) frames.push_back(speech(20 + part, 30 + i));
        if (part == 0) frames.insert(frames.end(), 50, voice::SILENT_FRAME);
        if (part == 1) frames.insert(frames.end(), 30, voice::SILENT_FRAME);
    }
    std::vector<uint8_t> file = voice::writeSot(frames);
    CHECK_EQ(file.size(), voice::HEADER_BYTES + 32 * 6);
    auto byteOf = [](uint64_t codedFrame) { return voice::HEADER_BYTES + codedFrame * 6; };

    // Lose file frames 8 to 12 (the last 2 of part 0, its pause, 2 of part 1).
    voice::Clip clip;
    std::vector<voice::ByteRange> missing = {{byteOf(8) + 1, byteOf(13) - 3}};
    CHECK(voice::readSot(file.data(), file.size(), file.size(), missing, clip) == voice::ReadResult::Ok);
    CHECK(!clip.whole);
    CHECK_EQ(clip.frames.size(), frames.size());
    // The pause's 50 frames went back into the hole: everything after it
    // is in its place.
    for (size_t i = 0; i < frames.size(); i++)
    {
        const bool lost = i >= 8 && i < 8 + 2 + 50 + 2;
        CHECK_EQ((int)clip.good[i], lost ? 0 : 1);
        if (!lost) CHECK(clip.frames[i] == frames[i]);
    }
    voice::PitchStrip strip = voice::pitchStrip(clip, (int)frames.size());
    CHECK(strip.columns[9].lost && !strip.columns[7].lost && !strip.columns[70].lost);

    // Two holes, each with a pause in: what they lost is shared out, and
    // the clip still runs to its length.
    missing = {{byteOf(10), byteOf(11)}, {byteOf(21), byteOf(22)}};
    CHECK(voice::readSot(file.data(), file.size(), file.size(), missing, clip) == voice::ReadResult::Ok);
    CHECK_EQ(clip.frames.size(), frames.size());
    int lost = 0;
    for (uint8_t g : clip.good) lost += g == 0;
    CHECK_EQ(lost, 80);
    CHECK(clip.frames.back() == frames.back());

    // A hole in the header: nothing to read.
    CHECK(voice::readSot(file.data(), file.size(), file.size(), {{3, 4}}, clip) == voice::ReadResult::Short);
}

void testRefusals()
{
    std::vector<uint8_t> good = voice::writeSot({speech(20), voice::SILENT_FRAME, voice::SILENT_FRAME, speech(21)});
    voice::Clip clip;
    std::vector<uint8_t> bad = good;
    bad[0] = 'X';
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::NotSot);
    CHECK(!voice::isSot(bad.data(), bad.size()));
    bad = good;
    bad[2] = 2;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::UnknownCodec);
    bad = good;
    bad[3] = 0x01;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::UnknownFlags);
    bad = good;
    bad[4] = 0xFF;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::TooLong);
    // The header says more frames, or fewer, than the file holds.
    bad = good;
    bad[6] = 5;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::Malformed);
    bad[6] = 3;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::Malformed);
    // A part frame at the end of a whole file.
    bad = good;
    bad.push_back(0);
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::Malformed);
    // A run longer than the clip.
    bad = good;
    bad[voice::HEADER_BYTES + 6 + 2] = 0x40;
    CHECK(voice::readSot(bad, clip) == voice::ReadResult::Malformed);
    CHECK(voice::readSot(good, clip) == voice::ReadResult::Ok);
}

void testFuzz()
{
    std::vector<uint8_t> base = voice::makeSot(loadClip());
    std::mt19937 rng(1234);
    int ok = 0, refused = 0;
    for (int round = 0; round < 3000; round++)
    {
        std::vector<uint8_t> f = base;
        if (round % 3 == 0)
        {
            f.resize(rng() % 200);
            for (uint8_t& b : f) b = (uint8_t)rng();
            if (f.size() >= 2 && round % 2 == 0)
            {
                f[0] = 'S';
                f[1] = 'o';
            }
        }
        else
        {
            int flips = 1 + (int)(rng() % 8);
            for (int k = 0; k < flips && !f.empty(); k++) f[rng() % f.size()] ^= (uint8_t)(1u << (rng() % 8));
            if (round % 5 == 0) f.resize(rng() % (f.size() + 1));
        }
        voice::Clip clip;
        uint64_t have = f.empty() ? 0 : rng() % (f.size() + 1);
        std::vector<voice::ByteRange> missing;
        if (round % 4 == 0 && !f.empty())
        {
            uint64_t a = rng() % f.size();
            missing.push_back({a, a + rng() % 40});
        }
        voice::ReadResult r = voice::readSot(f.data(), f.size(), round % 2 ? f.size() : have, missing, clip);
        if (r == voice::ReadResult::Ok)
        {
            ok++;
            CHECK(clip.frames.size() == clip.good.size());
            CHECK(clip.frames.size() <= clip.length);
            CHECK(clip.length <= voice::MAX_CLIP_FRAMES);
            voice::PitchStrip strip = voice::pitchStrip(clip, 64);
            CHECK_EQ(strip.columns.size(), (size_t)64);
        }
        else
            refused++;
        voice::playDelaySeconds(f, 100.0);
    }
    std::printf("fuzz: %d read, %d refused\n", ok, refused);
    CHECK(ok > 0 && refused > 0);
}

void testStripAndSeek()
{
    std::vector<Frame> frames;
    for (int i = 0; i < 50; i++) frames.push_back(speech(25, 20, voice::Voicing::Voiced));
    frames.insert(frames.end(), 50, voice::SILENT_FRAME);
    for (int i = 0; i < 50; i++) frames.push_back(speech(20, 60, voice::Voicing::Voiced));
    for (int i = 0; i < 50; i++) frames.push_back(speech(15, 0, voice::Voicing::Unvoiced));
    voice::Clip clip;
    CHECK(voice::readSot(voice::writeSot(frames), clip) == voice::ReadResult::Ok);
    voice::PitchStrip strip = voice::pitchStrip(clip, 4);
    CHECK(strip.columns[0].voiced && !strip.columns[0].silent);
    CHECK(strip.columns[1].silent && !strip.columns[1].voiced);
    CHECK(strip.columns[2].voiced && strip.height(strip.columns[2]) > strip.height(strip.columns[0]));
    CHECK(!strip.columns[3].voiced && !strip.columns[3].silent);
    CHECK_EQ(strip.height(strip.columns[3]), 0.0);
    CHECK(strip.thickness(strip.columns[0]) > strip.thickness(strip.columns[3]));
    CHECK_EQ(strip.thickness(strip.columns[1]), 0.0);
    // More columns than frames: every column has a frame.
    voice::Clip tiny;
    CHECK(voice::readSot(voice::writeSot({speech(20), speech(21)}), tiny) == voice::ReadResult::Ok);
    voice::PitchStrip wide = voice::pitchStrip(tiny, 9);
    for (const voice::StripColumn& c : wide.columns) CHECK(c.arrived && !c.silent);

    CHECK_EQ(voice::frameAt(clip, 0, 100), (uint32_t)0);
    CHECK_EQ(voice::frameAt(clip, 50, 100), (uint32_t)100);
    CHECK_EQ(voice::frameAt(clip, 100, 100), (uint32_t)199);
    CHECK_EQ(voice::frameAt(clip, -5, 100), (uint32_t)0);
    CHECK_EQ(voice::frameAt(voice::Clip(), 5, 100), (uint32_t)0);
}

void testAirtime()
{
    // Ten seconds of speech, no pauses: 3,000 bytes and a header.
    std::vector<Frame> talk(500, speech(22));
    std::vector<uint8_t> file = voice::writeSot(talk);
    CHECK_EQ(file.size(), (size_t)3007);

    // A link faster than speech plays at once, bar the first two frames.
    double fast = voice::playDelaySeconds(file, 1e6);
    CHECK(fast >= 0 && fast < 0.03);
    // At half speed (150 B/s) the clip takes 20 s to come and plays 10:
    // wait about 10 s.
    double slow = voice::playDelaySeconds(file, 150.0);
    CHECK(slow > 9.9 && slow < 10.2);
    // A long pause in the middle gives the link time to catch up.
    std::vector<Frame> paused(talk.begin(), talk.begin() + 250);
    paused.insert(paused.end(), 500, voice::SILENT_FRAME);
    paused.insert(paused.end(), talk.begin() + 250, talk.end());
    double withPause = voice::playDelaySeconds(voice::writeSot(paused), 150.0);
    CHECK(withPause > 4.5 && withPause < slow);
    CHECK(voice::playDelaySeconds({1, 2, 3}, 150.0) < 0);

    CHECK_EQ(voice::sessionAirSeconds(3007, 0.0), 0.0);
    double session = voice::sessionAirSeconds(3007, 300.0);
    CHECK(session > 10.0 && session < 11.5);

    const std::vector<link::ModeInfo>& modes = link::knownModes();
    CHECK(!modes.empty());
    if (modes.empty()) return;
    double air = voice::broadcastAirSeconds(3007, "AG7EW-1.sot", "AG7EW", modes.front());
    CHECK(air > 0);
    CHECK(voice::broadcastAirSeconds(6014, "AG7EW-1.sot", "AG7EW", modes.front()) > air);
}

void testProtocolAndFormat()
{
    using namespace protocol;
    CHECK_EQ((int)FILE_VOICE, 3);
    CHECK(isVoiceKind(FILE_VOICE) && !isVoiceKind(FILE_PICTURE) && !isPictureKind(FILE_VOICE));
    Record r;
    r.header.callsign = "AG7EW";
    r.header.seq = 9;
    FileOffer offer;
    offer.size = 3007;
    offer.name = "AG7EW-1.sot";
    offer.kind = FILE_VOICE;
    r.body = offer;
    std::vector<uint8_t> bytes;
    CHECK(encodeRecord(r, bytes));
    Record back;
    const uint8_t* at = bytes.data();
    CHECK(decodeRecord(at, bytes.data() + bytes.size(), back) == DecodeResult::Ok);
    CHECK_EQ((int)bytes.back(), (int)FILE_VOICE);
    const FileOffer* o = back.as<FileOffer>();
    CHECK(o != nullptr && o->kind == FILE_VOICE);

    CHECK_EQ((int)voice::codecOfFormat("sotto v3.6, shape 9-9-8-8"), 1);
    CHECK_EQ((int)voice::codecOfFormat("sotto v3.5, shape 12-11-11"), 0);
    CHECK_EQ((int)voice::codecOfFormat(""), 0);
}

} // namespace

int main()
{
    testFields();
    testWriteAndRead();
    testGateAndTrim();
    testRealClip();
    testPrefix();
    testHoles();
    testRefusals();
    testFuzz();
    testStripAndSeek();
    testAirtime();
    testProtocolAndFormat();
    return ragtime_test::finish();
}
