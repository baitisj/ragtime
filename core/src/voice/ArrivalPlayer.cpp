#include "ragtime/voice/ArrivalPlayer.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "ragtime/voice/Wav.h"

namespace ragtime
{
namespace voice
{

namespace
{

// sotto's decoder is quiet once it has given out this many lost frames'
// sound in a row (the first lost frame holds the sound, then 6 dB down
// each 20 ms).
int quietAfterLost(const Clip& clip)
{
    const Codec* c = codecOf(clip.codec);
    return c ? c->quietAfterLost : 3;
}

int speechBytesPerSecond(const Clip& clip)
{
    const Codec* c = codecOf(clip.codec);
    return c ? c->bytesPerSecond() : SPEECH_BYTES_PER_SECOND;
}

} // namespace

ArrivalPlayer::ArrivalPlayer(FrameDecoder decoder) : ArrivalPlayer(std::move(decoder), Settings()) {}

ArrivalPlayer::ArrivalPlayer(FrameDecoder decoder, Settings settings)
    : decoder_(std::move(decoder)), settings_(settings)
{
    if (settings_.window < 2) settings_.window = 2;
    if (decoder_.delay < 1) decoder_.delay = 1;
    resetHeld();
}

void ArrivalPlayer::resetHeld()
{
    held_.assign((size_t)decoder_.delay, Held::Lead);
    lostRun_ = 0;
}

void ArrivalPlayer::setExpectedRate(double bytesPerSecond)
{
    expectedRate_ = bytesPerSecond > 0 ? bytesPerSecond : 0.0;
}

void ArrivalPlayer::setExpectedGap(double seconds)
{
    if (seconds >= 0) settings_.gapSeconds = seconds;
}

ReadResult ArrivalPlayer::arrived(const std::vector<uint8_t>& bytes, uint64_t size, uint64_t nowMs)
{
    return arrived(bytes.data(), size, bytes.size(), nowMs);
}

ReadResult ArrivalPlayer::arrived(const uint8_t* bytes, uint64_t size, uint64_t have, uint64_t nowMs)
{
    have = std::min(have, size);
    if (have < have_) return read_;     // never backwards: the same bytes, longer
    if (have > have_ || !haveFirst_)
    {
        Arrival a{nowMs, have};
        if (!haveFirst_)
        {
            first_ = a;
            haveFirst_ = true;
        }
        arrivals_.push_back(a);
        while (arrivals_.size() > settings_.window) arrivals_.pop_front();
    }
    size_ = size;
    have_ = have;

    Clip clip;
    ReadResult r = readSot(bytes, size, have, std::vector<ByteRange>(), clip);
    if (r == ReadResult::Ok)
    {
        // What has played stays as it was: a prefix only grows.
        if (clip.frames.size() >= clip_.frames.size()) clip_ = std::move(clip);
        if (frame_.size() != frameSamplesOf(clip_))
        {
            frame_.assign(frameSamplesOf(clip_), 0);
            frameAt_ = frame_.size();
        }
    }
    else if (read_ == ReadResult::Ok)
    {
        return read_;   // read before; a short read now changes nothing
    }
    read_ = r;
    if ((read_ != ReadResult::Ok && read_ != ReadResult::Short) || (read_ == ReadResult::Ok && clip_.length == 0))
    {
        // Not a clip we can play: it never will be.
        state_ = State::Done;
    }
    return read_;
}

void ArrivalPlayer::ended()
{
    ended_ = true;
    if (read_ != ReadResult::Ok || clip_.frames.empty())
    {
        if (state_ != State::Playing) state_ = State::Done;
    }
}

double ArrivalPlayer::rate() const
{
    if (arrivals_.size() >= 2)
    {
        const Arrival& last = arrivals_.back();
        double best = 0.0;
        bool have = false;
        // Over everything since the first arrival, and over the last few:
        // the slower of the two.
        for (const Arrival* from : {&first_, &arrivals_.front()})
        {
            if (last.ms <= from->ms + 500) continue;
            double r = (double)(last.have - from->have) * 1000.0 / (double)(last.ms - from->ms);
            best = have ? std::min(best, r) : r;
            have = true;
        }
        if (have) return best;
    }
    return expectedRate_;
}

double ArrivalPlayer::gap() const
{
    if (arrivals_.size() < 2) return settings_.gapSeconds;
    uint64_t longest = 0;
    for (size_t i = 1; i < arrivals_.size(); i++)
        longest = std::max(longest, arrivals_[i].ms - arrivals_[i - 1].ms);
    return longest / 1000.0;
}

size_t ArrivalPlayer::inHand() const
{
    // Each frame fed gives the sound of the front of what the decoder
    // holds; at the clip's end (flushed, or faded when the bytes stopped
    // short) all it holds comes out too.
    const size_t n = std::min(clip_.frames.size(), clip_.good.size());
    const bool complete = n >= clip_.length || ended_;
    const size_t toFeed = n > fed_ ? n - fed_ : 0;
    const size_t outOfHeld = complete ? held_.size() : std::min(toFeed, held_.size());
    size_t real = 0;
    for (size_t i = 0; i < outOfHeld; i++)
        if (held_[i] == Held::Real) real++;
    return real + (complete ? toFeed : toFeed - outOfHeld);
}

double ArrivalPlayer::waitSeconds(uint64_t nowMs) const
{
    if (read_ != ReadResult::Ok) return -1.0;
    const size_t n = clip_.frames.size();
    if (n >= clip_.length || ended_) return 0.0;

    const double r = rate() * settings_.rateShare;
    if (!(r > 0)) return -1.0;
    const double remaining = (double)(size_ - have_);
    // The rest at worst is all speech, 6 bytes for each frame, coming at r
    // after the next gap; the speech in hand plays meanwhile.
    const double inHandSeconds = inHand() * clip_.frameSeconds();
    const double sinceLast = arrivals_.empty() ? 0.0 : (double)(nowMs - std::min(nowMs, arrivals_.back().ms)) / 1000.0;
    const double gapLeft = std::max(0.0, gap() - sinceLast);
    const double behind = std::max(0.0, remaining / r - remaining / speechBytesPerSecond(clip_));
    return std::max(0.0, gapLeft + behind + settings_.marginSeconds - inHandSeconds);
}

double ArrivalPlayer::startsIn(uint64_t nowMs) const
{
    if (state_ == State::Playing || state_ == State::Done) return 0.0;
    return waitSeconds(nowMs);
}

bool ArrivalPlayer::ready(uint64_t nowMs) const
{
    if (read_ != ReadResult::Ok || clip_.frames.empty()) return false;
    const bool complete = clip_.frames.size() >= clip_.length || ended_;
    if (inHand() == 0 && !complete) return false;
    if (state_ == State::Stalled && !complete && inHand() * clip_.frameSeconds() < settings_.resumeSeconds) return false;
    const double wait = waitSeconds(nowMs);
    return wait >= 0 && wait <= 0.0;
}

bool ArrivalPlayer::takeSound()
{
    const Held front = held_.front();
    held_.pop_front();
    if (front == Held::Lead) return false;
    if (front == Held::Real)
    {
        slot_++;
        lostRun_ = 0;
    }
    else
    {
        lostRun_++;
    }
    frameAt_ = 0;
    return true;
}

bool ArrivalPlayer::fadeStep()
{
    // Nothing fed yet: nothing to fade.
    if (std::all_of(held_.begin(), held_.end(), [](Held h) { return h == Held::Lead; })) return false;
    // A run of lost frames' sound: sotto has gone quiet.
    const Frame silence = silentFrame(clip_.codec);
    while (lostRun_ < quietAfterLost(clip_))
    {
        decoder_.decode(silence, false, frame_.data());
        held_.push_back(Held::Lost);
        if (takeSound()) return true;
    }
    return false;
}

bool ArrivalPlayer::nextFrame()
{
    const size_t n = std::min(clip_.frames.size(), clip_.good.size());
    while (fed_ < n)
    {
        decoder_.decode(clip_.frames[fed_], clip_.good[fed_] != 0, frame_.data());
        fed_++;
        held_.push_back(Held::Real);
        // The decoder's lead-in is no sound; after running dry, the lost
        // frames it still holds come out first, fading back in.
        if (takeSound()) return true;
    }
    if (fed_ >= clip_.length)
    {
        // The clip is all fed: what the decoder holds comes out.
        while (std::any_of(held_.begin(), held_.end(), [](Held h) { return h == Held::Real; }))
        {
            if (!decoder_.flush(frame_.data()))
            {
                held_.clear();
                return false;
            }
            if (takeSound()) return true;
        }
        return false;
    }
    // The bytes stop short of the clip.
    if (ended_) return fadeStep();
    return false;
}

size_t ArrivalPlayer::pull(int16_t* out, size_t n, uint64_t nowMs)
{
    size_t given = 0;
    while (given < n)
    {
        if (frameAt_ < frame_.size())
        {
            size_t take = std::min(n - given, frame_.size() - frameAt_);
            std::memcpy(out + given, frame_.data() + frameAt_, take * sizeof(int16_t));
            frameAt_ += take;
            given += take;
            continue;
        }
        if (state_ == State::Done) break;
        if (state_ == State::Waiting || state_ == State::Stalled)
        {
            // Ran dry: let sotto fade the last sound out before waiting.
            if (state_ == State::Stalled && fadeStep()) continue;
            if (ended_ && state_ == State::Stalled && inHand() == 0)
            {
                state_ = State::Done;
                break;
            }
            if (!ready(nowMs)) break;
            state_ = State::Playing;
        }
        if (nextFrame()) continue;
        if (fed_ >= clip_.length || ended_)
        {
            state_ = State::Done;
            break;
        }
        // Ran dry: sotto plays out what it holds and fades, then it waits
        // for more and comes back in from the quiet.
        state_ = State::Stalled;
        lostRun_ = 0;
        if (fadeStep()) continue;
        break;
    }
    return given;
}

bool ArrivalPlayer::seek(uint32_t frame)
{
    if (read_ != ReadResult::Ok || frame >= clip_.length) return false;
    const bool complete = clip_.frames.size() >= clip_.length || ended_;
    if (complete && frame >= clip_.frames.size()) return false;
    fed_ = frame;
    slot_ = frame;
    resetHeld();
    frameAt_ = frame_.size();
    // A clip all here plays from there at once; one still coming waits
    // until the bytes will keep ahead from there.
    state_ = complete ? State::Playing : State::Waiting;
    return true;
}

std::vector<uint8_t> wavFile(const std::vector<int16_t>& samples, int rate)
{
    const uint32_t dataBytes = (uint32_t)(samples.size() * 2);
    std::vector<uint8_t> f;
    f.reserve(44 + dataBytes);
    auto text = [&](const char* s) { f.insert(f.end(), s, s + 4); };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) f.push_back((uint8_t)(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        f.push_back((uint8_t)v);
        f.push_back((uint8_t)(v >> 8));
    };
    text("RIFF");
    u32(36 + dataBytes);
    text("WAVE");
    text("fmt ");
    u32(16);
    u16(1);                     // PCM
    u16(1);                     // mono
    u32((uint32_t)rate);
    u32((uint32_t)rate * 2);    // bytes a second
    u16(2);                     // bytes a sample
    u16(16);
    text("data");
    u32(dataBytes);
    for (int16_t s : samples) u16((uint16_t)s);
    return f;
}

} // namespace voice
} // namespace ragtime
