// Watching a clip on the speaker by the clock (docs/STAGE1.md, "A clip's
// end by the clock"). A clip ends when the speaker says it has been heard
// (PipeWire's drained, WASAPI's empty buffer). A speaker that never says
// so, or stops taking sound part way, left the card showing Stop, and the
// next clip waiting, until something else stopped it (Jeff, 2026-11-03,
// on an xrdp speaker). The watch is fed how many samples the speaker has
// taken, each voice tick, and says when to give up waiting:
//
// - Ended: every sample has been taken, and END_GRACE_MS have passed
//   with no word from the speaker. The clip counts as heard; its sound
//   is left to play out.
// - Stalled: there was sound to take, and the speaker has taken none of
//   it for STALL_MS. The clip stops, and the operator is told why.
//
// Socket-free and not thread safe: the window's voice tick calls it.
#ifndef RAGTIME_VOICE_PLAY_WATCH_H
#define RAGTIME_VOICE_PLAY_WATCH_H

#include <cstdint>

namespace ragtime
{
namespace voice
{

class PlayWatch
{
public:
    // After the speaker has taken the last sample: room for what it
    // holds (PipeWire's 100 ms, a remote desktop's own buffer), and then
    // some, before the clock ends the clip.
    static constexpr uint64_t END_GRACE_MS = 2500;
    // A speaker that takes nothing for this long, with sound waiting for
    // it, has stopped. Long enough for a sink to wake from suspend.
    static constexpr uint64_t STALL_MS = 6000;

    enum class Verdict
    {
        Playing,
        Ended,
        Stalled,
    };

    // A clip starts. total is its length in samples, or 0 while it is
    // still coming (a clip playing as it arrives; ended() gives it).
    void start(uint64_t nowMs, uint64_t total);
    // A clip coming in pieces has had all it will get: total samples.
    void ended(uint64_t total);
    // Nothing playing.
    void clear() { running_ = false; }
    bool running() const { return running_; }

    // taken: samples the speaker has taken so far. ready: samples it has
    // been offered (the whole clip, or what has been handed over of one
    // still coming).
    Verdict check(uint64_t nowMs, uint64_t taken, uint64_t ready);

private:
    bool running_ = false;
    uint64_t total_ = 0;
    bool known_ = false;  // total_ is the clip's length
    uint64_t taken_ = 0;
    uint64_t movedMs_ = 0;
    uint64_t allTakenMs_ = 0;  // 0 until every sample has been taken
};

} // namespace voice
} // namespace ragtime

#endif
