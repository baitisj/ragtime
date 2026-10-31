// voice::ArrivalPlayer: a voice message played while its bytes still come
// in a ragchew, and voice::wavFile.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Check.h"
#include "ragtime/voice/ArrivalPlayer.h"
#include "ragtime/voice/Voice.h"
#include "ragtime/voice/Wav.h"

using namespace ragtime::voice;

namespace
{

// A pretend sotto: each frame's sound is its pitch code + 1 in every
// sample, a frame late as sotto's is; for a lost frame it gives 1000 + how
// many lost in a row (the hold and fade), quiet (0) after 4.
struct FakeSotto
{
    int16_t out = 0;        // the sound the next call gives
    int16_t last = 0;
    int lost = 0;
    int calls = 0;

    FrameDecoder decoder()
    {
        FrameDecoder d;
        d.decode = [this](const Frame& f, bool good, int16_t* pcm) {
            calls++;
            std::fill(pcm, pcm + FRAME_SAMPLES, out);
            if (good)
            {
                lost = 0;
                last = (int16_t)(pitchCodeOf(f) + 1);
                out = last;
            }
            else
            {
                lost++;
                // The call after a lost frame gives the last good frame's
                // own sound; the next ones its hold and fade.
                out = lost <= 4 ? (int16_t)(1000 + lost) : (int16_t)0;
            }
        };
        d.flush = [this](int16_t* pcm) { std::fill(pcm, pcm + FRAME_SAMPLES, out); };
        return d;
    }
};

// A clip of n frames, frame i at pitch code i % 120, with no pauses.
std::vector<uint8_t> clipFile(int n)
{
    std::vector<Frame> frames;
    for (int i = 0; i < n; i++) frames.push_back(makeFrame(Voicing::Voiced, i % 120, 20, (uint64_t)i * 7919));
    PauseGate gate;
    gate.enabled = false;
    return makeSot(frames, gate);
}

// Bytes arriving: at ms, the first have.
struct Step
{
    uint64_t ms;
    uint64_t have;
};

struct Run
{
    std::vector<int> heard;     // each frame of sound given, its value
    int stalls = 0;
    uint64_t startedMs = 0;     // when the first sound came
    bool done = false;
    uint64_t doneMs = 0;
};

// Plays file arriving as steps say, pulling one frame of sound each
// 20 ms, until Done or untilMs. endAtMs: when ended() is called (0: when
// the last byte is in).
Run play(ArrivalPlayer& player, const std::vector<uint8_t>& file, const std::vector<Step>& steps, uint64_t untilMs,
         uint64_t endAtMs = 0)
{
    Run run;
    size_t next = 0;
    bool started = false;
    ArrivalPlayer::State was = player.state();
    uint64_t have = 0;
    for (uint64_t ms = 0; ms <= untilMs; ms += FRAME_MS)
    {
        while (next < steps.size() && steps[next].ms <= ms)
        {
            have = std::min<uint64_t>(steps[next].have, file.size());
            player.arrived(file.data(), file.size(), have, ms);
            next++;
            if (endAtMs == 0 && have == file.size()) player.ended();
        }
        if (endAtMs != 0 && ms >= endAtMs && !player.hasEnded()) player.ended();
        int16_t pcm[FRAME_SAMPLES];
        size_t got = player.pull(pcm, FRAME_SAMPLES, ms);
        if (got > 0)
        {
            if (!started) run.startedMs = ms;
            started = true;
            run.heard.push_back(pcm[0]);
        }
        if (player.state() == ArrivalPlayer::State::Stalled && was != ArrivalPlayer::State::Stalled) run.stalls++;
        was = player.state();
        if (player.state() == ArrivalPlayer::State::Done)
        {
            run.done = true;
            run.doneMs = ms;
            break;
        }
    }
    return run;
}

// The real frames heard, in order (fades and quiet left out).
std::vector<int> spoken(const Run& run)
{
    std::vector<int> v;
    for (int s : run.heard)
        if (s > 0 && s < 1000) v.push_back(s);
    return v;
}

std::vector<int> expected(int n)
{
    std::vector<int> v;
    for (int i = 0; i < n; i++) v.push_back(i % 120 + 1);
    return v;
}

// Bytes coming a burst at a time: every gapMs, bytesPerBurst more, the
// first burst in at firstMs.
std::vector<Step> bursts(uint64_t size, uint64_t bytesPerBurst, uint64_t gapMs, uint64_t firstMs = 0)
{
    std::vector<Step> steps;
    uint64_t have = 0, ms = firstMs;
    while (have < size)
    {
        have = std::min(size, have + bytesPerBurst);
        steps.push_back({ms, have});
        ms += gapMs;
    }
    return steps;
}

void testWholeAtOnce()
{
    // A clip that is all in plays at once, every frame in order, ending
    // with the last frame's sound (flushed).
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    std::vector<uint8_t> file = clipFile(150);
    Run run = play(player, file, {{0, file.size()}}, 10000);
    CHECK(run.done);
    CHECK_EQ(run.startedMs, (uint64_t)0);
    CHECK_EQ(run.stalls, 0);
    CHECK(run.heard == expected(150));
    CHECK_EQ(player.playedFrames(), 150u);
}

void testFastLinkStartsAfterOneGap()
{
    // A 10 s clip (3,007 bytes) at gear 7, about 400 bytes a second
    // given in 1,600-byte bursts every 4 s: faster than the speech, so it
    // waits only for the gap after the first burst, less the 5 s that
    // burst holds, and never stalls.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(400);
    player.setExpectedGap(4.0);
    std::vector<uint8_t> file = clipFile(500);
    Run run = play(player, file, bursts(file.size(), 1600, 4000), 60000);
    CHECK(run.done);
    CHECK_EQ(run.stalls, 0);
    CHECK(spoken(run) == expected(500));
    CHECK(run.heard == expected(500));
    CHECK_EQ(run.startedMs, (uint64_t)0);
}

void testSlowLinkWaitsThenNeverStalls()
{
    // At gear 6 with the session's other traffic, 150 bytes a second in
    // 600-byte bursts every 4 s: half the speech's rate. A 10 s clip
    // needs about 10 s in hand before it starts.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(150);
    player.setExpectedGap(4.0);
    std::vector<uint8_t> file = clipFile(500);
    std::vector<Step> steps = bursts(file.size(), 600, 4000);
    Run run = play(player, file, steps, 120000);
    CHECK(run.done);
    CHECK_EQ(run.stalls, 0);
    CHECK(run.heard == expected(500));
    // The sum says S/R - S/P = 3007/150 - 10 = 10 s; with the bursts it
    // can't start before the last burst less the clip's length.
    const double lastBurst = steps.back().ms / 1000.0;
    CHECK(run.startedMs / 1000.0 >= lastBurst - 10.0);
    // and not much later than that plus a gap and the margins.
    CHECK(run.startedMs / 1000.0 <= lastBurst - 10.0 + 4.0 + 4.0);
    std::printf("slow link: last burst %.1f s, started %.1f s, done %.1f s\n", lastBurst, run.startedMs / 1000.0,
                run.doneMs / 1000.0);
}

void testNoRateWaitsForSecondArrival()
{
    // With no rate to go on, it waits for a second burst to measure one.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    std::vector<uint8_t> file = clipFile(500);
    Run run = play(player, file, bursts(file.size(), 1600, 4000), 60000);
    CHECK(run.done);
    CHECK_EQ(run.stalls, 0);
    CHECK(run.startedMs >= 4000);
    CHECK(run.heard == expected(500));
}

void testUnderrunFadesAndLosesNothing()
{
    // The link fades: the first bursts promise gear 7, then nothing comes
    // for 20 s. It runs dry, lets sotto hold and fade the sound (frames
    // marked 1001 to 1003, and 1004 fading back in), waits, and carries on where it stopped, every
    // frame heard once and in order.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(400);
    player.setExpectedGap(2.0);
    std::vector<uint8_t> file = clipFile(1000);
    std::vector<Step> steps = {{0, 800}, {2000, 1600}, {24000, 3000}, {26000, 4500}, {28000, file.size()}};
    Run run = play(player, file, steps, 120000);
    CHECK(run.done);
    CHECK(run.stalls >= 1);
    CHECK(spoken(run) == expected(1000));
    bool faded = false;
    for (size_t i = 0; i + 2 < run.heard.size(); i++)
        if (run.heard[i] == 1001 && run.heard[i + 1] == 1002 && run.heard[i + 2] == 1003) faded = true;
    CHECK(faded);
}

void testStopSignPlaysWhatCameThenFades()
{
    // The stop sign at 3 s: the bytes stop, and it plays what came (the
    // frames in the first 1,500 bytes) and fades out, then is Done.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(400);
    player.setExpectedGap(2.0);
    std::vector<uint8_t> file = clipFile(1000);
    std::vector<Step> steps = {{0, 700}, {2000, 1500}};
    Run run = play(player, file, steps, 60000, 3000);
    CHECK(run.done);
    const int frames = (int)((1500 - HEADER_BYTES) / FRAME_BYTES);
    CHECK(spoken(run) == expected(frames));
    CHECK_EQ(player.playedFrames(), (uint32_t)frames);
    CHECK_EQ(run.heard.back(), 1003);
}

void testStoppedWhileWaitingPlaysAtOnce()
{
    // Ended while still waiting to start: no reason to wait any longer.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(100);
    std::vector<uint8_t> file = clipFile(1000);
    Run run = play(player, file, {{0, 607}}, 30000, 1000);
    CHECK(run.done);
    CHECK_EQ(run.startedMs, (uint64_t)1000);
    CHECK(spoken(run) == expected(100));
}

void testNotAClip()
{
    // Not a clip we can play (a codec from a later Ragtime): it never
    // starts.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    std::vector<uint8_t> file = clipFile(50);
    file[2] = 99;
    CHECK(player.arrived(file, file.size(), 0) == ReadResult::UnknownCodec);
    int16_t pcm[FRAME_SAMPLES];
    CHECK_EQ(player.pull(pcm, FRAME_SAMPLES, 0), (size_t)0);
    CHECK(player.state() == ArrivalPlayer::State::Done);
    CHECK_EQ(sotto.calls, 0);
}

void testShortHeaderWaits()
{
    // Five bytes in: not even the header. It waits.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    std::vector<uint8_t> file = clipFile(50);
    CHECK(player.arrived(file.data(), file.size(), 5, 0) == ReadResult::Short);
    int16_t pcm[FRAME_SAMPLES];
    CHECK_EQ(player.pull(pcm, FRAME_SAMPLES, 0), (size_t)0);
    CHECK(player.state() == ArrivalPlayer::State::Waiting);
    CHECK(player.arrived(file, file.size(), 100) == ReadResult::Ok);
    player.ended();
    CHECK_EQ(player.pull(pcm, FRAME_SAMPLES, 100), (size_t)FRAME_SAMPLES);
    CHECK_EQ(pcm[0], 1);
}

void testSeek()
{
    // A click on the strip plays from that frame.
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    std::vector<uint8_t> file = clipFile(100);
    player.arrived(file, file.size(), 0);
    player.ended();
    int16_t pcm[FRAME_SAMPLES];
    player.pull(pcm, FRAME_SAMPLES, 0);
    CHECK_EQ(pcm[0], 1);
    CHECK(player.seek(60));
    CHECK(!player.seek(100));
    player.pull(pcm, FRAME_SAMPLES, 20);
    CHECK_EQ(pcm[0], 61);
    CHECK_EQ(player.playedFrames(), 61u);
}

void testSilenceRunsGiveTheLinkTime()
{
    // A pause of 3 s is one frame in the file, so a slow link gets ahead
    // while it plays: it starts sooner than the same bytes of speech would.
    std::vector<Frame> frames;
    for (int i = 0; i < 100; i++) frames.push_back(makeFrame(Voicing::Voiced, i % 120, 20, 1));
    for (int i = 0; i < 150; i++) frames.push_back(SILENT_FRAME);
    for (int i = 0; i < 100; i++) frames.push_back(makeFrame(Voicing::Voiced, (100 + i) % 120, 20, 1));
    PauseGate gate;
    gate.enabled = false;
    std::vector<uint8_t> file = makeSot(frames, gate);
    CHECK_EQ(file.size(), HEADER_BYTES + 201 * (size_t)FRAME_BYTES);
    FakeSotto sotto;
    ArrivalPlayer player(sotto.decoder());
    player.setExpectedRate(150);
    player.setExpectedGap(2.0);
    Run run = play(player, file, bursts(file.size(), 300, 2000), 120000);
    CHECK(run.done);
    CHECK_EQ(run.stalls, 0);
    CHECK_EQ((int)run.heard.size(), 350);
}

void testWav()
{
    std::vector<int16_t> samples = {0, 1, -1, 32767, -32768};
    std::vector<uint8_t> f = wavFile(samples);
    CHECK_EQ(f.size(), (size_t)(44 + 10));
    CHECK(f[0] == 'R' && f[1] == 'I' && f[2] == 'F' && f[3] == 'F');
    CHECK(f[8] == 'W' && f[12] == 'f' && f[36] == 'd');
    CHECK_EQ((int)f[4], 36 + 10);
    CHECK_EQ((int)f[22], 1);                              // mono
    CHECK_EQ((int)(f[24] | (f[25] << 8)), 8000);          // 8 kHz
    CHECK_EQ((int)f[34], 16);                             // 16-bit
    CHECK_EQ((int)f[40], 10);                             // data bytes
    CHECK_EQ((int)f[46], 1);
    CHECK_EQ((int)f[48], 0xFF);
    CHECK_EQ((int)f[49], 0xFF);
    CHECK_EQ((int)f[51], 0x7F);
    CHECK_EQ((int)f[53], 0x80);
}

} // namespace

int main()
{
    testWholeAtOnce();
    testFastLinkStartsAfterOneGap();
    testSlowLinkWaitsThenNeverStalls();
    testNoRateWaitsForSecondArrival();
    testUnderrunFadesAndLosesNothing();
    testStopSignPlaysWhatCameThenFades();
    testStoppedWhileWaitingPlaysAtOnce();
    testNotAClip();
    testShortHeaderWaits();
    testSeek();
    testSilenceRunsGiveTheLinkTime();
    testWav();
    return ragtime_test::finish();
}
