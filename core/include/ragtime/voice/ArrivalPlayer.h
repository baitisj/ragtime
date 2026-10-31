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
// frame is lost, so it does what it does for a lost frame on 2 m: holds
// the sound 40 ms, fades 6 dB each 20 ms and goes quiet after 80 ms. The
// player then waits by the same rule and carries on where it stopped,
// losing nothing. When the transfer ends short (the stop sign, a cancel, a
// dropped link) it plays what came and fades out the same way.
//
// No audio device here: the app pulls samples at sotto's 8 kHz and gives
// the time. The decoder is two calls, so tests give a fake one and the app
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

// sotto's sound: 8 kHz, 16-bit, 160 samples a frame.
constexpr int PLAY_RATE = 8000;
constexpr int FRAME_SAMPLES = PLAY_RATE * FRAME_MS / 1000;

// sotto's decoder, as libsotto's sotto_decode and sotto_dec_flush. decode
// takes a frame, or with good false covers a lost one, and gives 160
// samples a frame behind: the first call gives silence, and flush gives
// the last frame's sound.
struct FrameDecoder
{
    std::function<void(const Frame& frame, bool good, int16_t* out)> decode;
    std::function<void(int16_t* out)> flush;
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

    // Play from frame (a click on the card's strip), if it has come; true
    // if it moved.
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

    // Seconds it must still wait before (re)starting, from nowMs, with
    // slots of sound in hand; negative if it can't tell.
    double waitSeconds(uint64_t nowMs) const;
    // Sound slots playable from slot_ with what has come.
    size_t inHand() const;
    // Decodes the next 160 samples into frame_, or returns false if it
    // can't without running dry.
    bool nextFrame();
    // One more lost frame into frame_, for sotto to fade; false once it
    // has gone quiet.
    bool fadeStep();
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

    // The decoder's place: fed_ frames given to it, the first of whose
    // sound (silence) is thrown away, so slot_ sound frames have played.
    uint32_t fed_ = 0;
    uint32_t slot_ = 0;
    bool primed_ = false;
    bool flushed_ = false;
    // The frame of sound being given out, and how far.
    int16_t frame_[FRAME_SAMPLES] = {};
    size_t frameAt_ = FRAME_SAMPLES;
    // Lost frames fed since it ran dry or the bytes stopped short (sotto
    // is quiet after 4), and whether the next frame decoded is the fade
    // back in after running dry, which is no slot of the clip.
    int fading_ = 0;
    bool resuming_ = false;
};

} // namespace voice
} // namespace ragtime

#endif
