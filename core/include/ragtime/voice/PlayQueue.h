// Which voice message plays, and which wait (docs/STAGE1.md, "Voice
// cards"). One clip plays at a time. Pressing play on a card plays it at
// once, stopping the one playing; while the microphone deck is open, voice
// that arrives plays by itself, in the order it came, each clip once.
// Holding Push to talk pauses whatever plays, and letting go carries it on
// from where it paused. Stop on the card playing stops it and empties the
// queue: the operator wants quiet.
//
// Clips are named by key, as the window's shelves name them ("b<id>" a
// broadcast file, "s<id>" a session transfer, "p<path>" one on disk). The
// queue only decides: each call that starts something returns what to
// start, and the window decodes and plays it, then tells the queue when it
// has finished. Socket-free and not thread safe.
#ifndef RAGTIME_VOICE_PLAY_QUEUE_H
#define RAGTIME_VOICE_PLAY_QUEUE_H

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>

namespace ragtime
{
namespace voice
{

class PlayQueue
{
public:
    // A clip to start, from this frame (20 ms each) of it.
    struct Start
    {
        std::string key;
        uint32_t fromFrame = 0;
    };

    // A clip has come and can play. With autoPlay (the deck open, Play
    // arriving voice ticked) it waits its turn, and starts now if nothing
    // plays and Push to talk isn't held. A clip arrives once: a key seen
    // before, played or waiting, is left as it is.
    std::optional<Start> arrived(const std::string& key, bool autoPlay);

    // Play pressed on a card, or its strip clicked at fromFrame: whatever
    // plays stops and this plays now, even while Push to talk is held (the
    // operator asked for it). Clips waiting still wait, after it.
    Start play(const std::string& key, uint32_t fromFrame = 0);

    // Stop pressed on the card playing: nothing plays, nothing waits.
    void stop();

    // The clip playing has played to its end (or couldn't play): the next
    // waiting starts, unless Push to talk is held. A key that isn't
    // playing (one replaced, finishing late) changes nothing.
    std::optional<Start> finished(const std::string& key);

    // Push to talk pressed: the clip playing, at frame atFrame, pauses.
    // Returns true if one was playing (the window stops its sound).
    bool hold(uint32_t atFrame);
    // Push to talk let go: the paused clip carries on from where it
    // paused, or the next waiting starts.
    std::optional<Start> release();
    bool held() const { return held_; }

    // The deck has closed: arriving voice no longer plays by itself, so
    // what waits waits no more (the clip playing plays on).
    void clearWaiting();

    // A card is gone (its file removed): it neither plays nor waits.
    // Returns true if it was playing.
    bool forget(const std::string& key);

    // The clip playing ("" if none; while Push to talk holds it, still
    // the paused one, with paused() true).
    const std::string& playing() const { return playing_; }
    bool paused() const { return held_ && !playing_.empty(); }
    bool waiting(const std::string& key) const;
    size_t waitingCount() const { return waiting_.size(); }

private:
    std::optional<Start> next();

    std::string playing_;
    uint32_t pausedAt_ = 0;
    bool held_ = false;
    std::deque<std::string> waiting_;
    std::set<std::string> seen_;
};

} // namespace voice
} // namespace ragtime

#endif
