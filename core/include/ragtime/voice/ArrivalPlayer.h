// Playing a voice message while it is still arriving (docs/STAGE1.md,
// "Voice while it arrives"). A ragchew's stream is reliable and in order,
// so the far end can play a clip as its bytes come, if it starts late
// enough that the bytes keep ahead of the speech.
//
// The professor's sum: a clip of S bytes arriving at R bytes a second and
// played at P = 300 bytes a second can start after S/R - S/P seconds and
// never stall. A session's bytes don't come smoothly, though: they come a
// burst at a time, with the other station's turn between. So the player
// also waits out one gap between bursts, less what it already holds, and
// it measures R and the gap from the arrivals rather than trusting a
// figure. With R above P only the gap counts, and a clip of a few seconds
// starts as soon as its first burst is in.
//
// If it guesses wrong and runs dry, sotto's decoder is told the next
// frames are lost, so it does what it does for a lost frame on 2 m: plays
// out the frames it holds, holds the sound 40 ms, fades 6 dB each 20 ms
// and goes quiet after 80 ms. The
// player then waits by the same rule and carries on where it stopped,
// losing nothing. When the transfer ends short (the stop sign, a cancel, a
// dropped link) it plays what came and fades out the same way.
//
// No audio device here: the app pulls samples at 16 kHz and gives the
// time. The decoder is two calls, so tests give a fake one and the app
// libsotto.
#ifndef RAGTIME_VOICE_ARRIVALPLAYER_H
#define RAGTIME_VOICE_ARRIVALPLAYER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "ragtime/voice/Voice.h"

namespace ragtime
{
namespace voice
{

// Voice's sound: 16 kHz, 16-bit, 320 samples a 20 ms frame (sotto v3.8's
// sotto_decode16; codec 1 is decoded at 8 kHz and doubled).
constexpr int PLAY_RATE = 16000;
constexpr int FRAME_SAMPLES = PLAY_RATE * FRAME_MS / 1000;
// A codec's frame of sound: 320 samples at 2,400, up to 1,920 at 400.
inline size_t frameSamplesOf(const Clip& clip)
{
    return (size_t)(PLAY_RATE / 1000 * clip.frameMs());
}

// sotto's decoder, as voice::SottoDecoder gives it. decode takes a frame,
// or with good false covers a lost one, and gives a frame of sound (the
// clip's codec's: frameSamplesOf) delay frames behind: the first delay
// calls give silence (1 for sotto's 8 kHz decoder, SOTTO_DEC16_DELAY_FRAMES
// = 5 for the 16 kHz one, 4 at 1,200 and 600, 3 at 400). flush gives the frames
// it still holds, one a call, and false once it holds none.
struct FrameDecoder
{
    std::function<void(const Frame& frame, bool good, int16_t* out)> decode;
    std::function<bool(int16_t* out)> flush;
    int delay = 1;
};

class ArrivalPlayer
{
public:
    struct Settings
    {
        // Count on this share of the measured rate, for the bursts that
        // come slower than the ones before.
        double rateShare = 0.85;
        // And start this much later than the sum says.
        double marginSeconds = 0.5;
        // Until two arrivals give a gap, the gap between a session's
        // bursts is taken as this (the app gives the mode's two longest
        // bursts, as session pacing reckons them).
        double gapSeconds = 6.0;
        // After running dry, carry on only with this much in hand (or the
        // rest of the clip), so it doesn't stutter a frame at a time.
        double resumeSeconds = 1.0;
        // Rates and gaps from the last this many arrivals.
        size_t window = 6;
    };

    enum class State
    {
        Waiting,    // has too little to start without stalling
        Playing,
        Stalled,    // ran dry: faded out, waiting for more
        Done,       // played all it will get
    };

    explicit ArrivalPlayer(FrameDecoder decoder);
    ArrivalPlayer(FrameDecoder decoder, Settings settings);

    // Before the second arrival gives a rate, count on this one (bytes a
    // second, picture::sessionBytesPerSecond for the session's mode); 0
    // if none is known, and then the player waits for a second arrival or
    // the end.
    void setExpectedRate(double bytesPerSecond);
    // The gap between bursts to count on until arrivals show one.
    void setExpectedGap(double seconds);

    // The file's bytes as they stand: the first have of size, at nowMs.
    // Call again each time more have come (the same bytes, longer). Not Ok
    // if they don't read as a clip this Ragtime can play; then it never
    // starts.
    ReadResult arrived(const uint8_t* bytes, uint64_t size, uint64_t have, uint64_t nowMs);
    ReadResult arrived(const std::vector<uint8_t>& bytes, uint64_t size, uint64_t nowMs);

    // Nothing more will come: the clip is in, or it stopped short (the
    // stop sign, a cancel, the link gone). It plays what came, at once if
    // it was waiting, and fades out at the end.
    void ended();

    // Up to n samples of the clip's sound at PLAY_RATE, the fades
    // included; fewer, maybe none, when it waits (the speaker plays
    // silence meanwhile: audio::VoiceAudio::append) or is Done. nowMs is
    // the time on the same clock as arrived's, when these samples will
    // be heard.
    size_t pull(int16_t* out, size_t n, uint64_t nowMs);

    // Play from frame (a click on the card's strip, or carrying on after
    // Push to talk). In a clip all here it plays from there at once; in
    // one still coming it waits there, by the same rule, for the bytes to
    // keep ahead (frames not yet come included). False past the clip's
    // end, or past what came of one that ended short.
    bool seek(uint32_t frame);

    State state() const { return state_; }
    const Clip& clip() const { return clip_; }
    ReadResult readResult() const { return read_; }
    bool hasEnded() const { return ended_; }
    // Frames played, for the card's playhead.
    uint32_t playedFrames() const { return slot_; }
    // While Waiting or Stalled: how long until it expects to play, in
    // seconds, as things stand; negative if it can't tell yet (no rate).
    double startsIn(uint64_t nowMs) const;
    // The rate it counts on now (before rateShare), bytes a second; 0 if
    // none.
    double rate() const;
    // The longest gap between arrivals it has seen lately, or the
    // expected one, in seconds.
    double gap() const;

private:
    struct Arrival
    {
        uint64_t ms = 0;
        uint64_t have = 0;
    };

    // What the decoder holds, oldest first: each call's sound is the
    // front's, as the call pushes its own on the back.
    enum class Held : uint8_t
    {
        Lead,   // the silence before the first frame fed, thrown away
        Real,   // a frame of the clip: its sound is a slot
        Lost,   // fed while dry or past what came: sotto's fade
    };

    // Seconds it must still wait before (re)starting, from nowMs, with
    // slots of sound in hand; negative if it can't tell.
    double waitSeconds(uint64_t nowMs) const;
    // Sound slots playable from slot_ with what has come.
    size_t inHand() const;
    // Decodes the next frame of sound into frame_, or returns false if it
    // can't without running dry.
    bool nextFrame();
    // One more lost frame fed, for sotto to play out what it holds and
    // fade; false once it has gone quiet.
    bool fadeStep();
    // Takes the front of held_ for a call's sound just made into frame_:
    // false if that was Lead (thrown away), else true, counting a slot for
    // Real.
    bool takeSound();
    // Starts the decoder's pipe over (seek, the start).
    void resetHeld();
    // Waiting or Stalled: whether to play now.
    bool ready(uint64_t nowMs) const;

    FrameDecoder decoder_;
    Settings settings_;
    double expectedRate_ = 0.0;
    Clip clip_;
    ReadResult read_ = ReadResult::Short;
    uint64_t size_ = 0;
    uint64_t have_ = 0;
    std::deque<Arrival> arrivals_;
    Arrival first_;
    bool haveFirst_ = false;
    bool ended_ = false;
    State state_ = State::Waiting;

    // The decoder's place: fed_ frames of the clip given to it, held_
    // what it holds (decoder_.delay of them), and slot_ frames of the
    // clip's sound played.
    uint32_t fed_ = 0;
    uint32_t slot_ = 0;
    std::deque<Held> held_;
    // Lost frames' sound given out in a row (sotto is quiet after
    // Codec::quietAfterLost).
    int lostRun_ = 0;
    // The frame of sound being given out (sized for the clip's codec once
    // it reads), and how far.
    std::vector<int16_t> frame_;
    size_t frameAt_ = 0;
};

} // namespace voice
} // namespace ragtime

#endif
