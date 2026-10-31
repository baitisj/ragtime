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

// sotto's decoder is quiet after this many lost frames running (hold
// 40 ms, then 6 dB down each 20 ms).
constexpr int QUIET_AFTER_LOST = 4;

constexpr double FRAME_SECONDS = FRAME_MS / 1000.0;

} // namespace

ArrivalPlayer::ArrivalPlayer(FrameDecoder decoder) : ArrivalPlayer(std::move(decoder), Settings()) {}

ArrivalPlayer::ArrivalPlayer(FrameDecoder decoder, Settings settings)
    : decoder_(std::move(decoder)), settings_(settings)
{
    if (settings_.window < 2) settings_.window = 2;
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
    // Slot k's sound needs frame k + 1, or the end of the clip (flushed,
    // or faded when the bytes stopped short).
    const size_t n = clip_.frames.size();
    const bool complete = n >= clip_.length || ended_;
    const size_t playable = complete ? n : (n == 0 ? 0 : n - 1);
    return playable > slot_ ? playable - slot_ : 0;
}

double ArrivalPlayer::waitSeconds(uint64_t nowMs) const
{
    if (read_ != ReadResult::Ok) return -1.0;
    const size_t n = clip_.frames.size();
    if (n >= clip_.length || ended_) return 0.0;

    const double r = rate() * settings_.rateShare;
    if (!(r > 0)) return -1.0;
    const double remaining = (double)(size_ - have_);
    // The rest at worst is all speech, 6 bytes for each 20 ms, coming at
    // r after the next gap; the speech in hand plays meanwhile.
    const double inHandSeconds = inHand() * FRAME_SECONDS;
    const double sinceLast = arrivals_.empty() ? 0.0 : (double)(nowMs - std::min(nowMs, arrivals_.back().ms)) / 1000.0;
    const double gapLeft = std::max(0.0, gap() - sinceLast);
    const double behind = std::max(0.0, remaining / r - remaining / SPEECH_BYTES_PER_SECOND);
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
    if (state_ == State::Stalled && !complete && inHand() * FRAME_SECONDS < settings_.resumeSeconds) return false;
    const double wait = waitSeconds(nowMs);
    return wait >= 0 && wait <= 0.0;
}

bool ArrivalPlayer::fadeStep()
{
    if (!primed_ || fading_ >= QUIET_AFTER_LOST) return false;
    decoder_.decode(SILENT_FRAME, false, frame_);
    frameAt_ = 0;
    // The first lost frame still gives the last frame's own sound, held.
    if (fading_ == 0 && !resuming_) slot_++;
    fading_++;
    return true;
}

bool ArrivalPlayer::nextFrame()
{
    const size_t n = std::min(clip_.frames.size(), clip_.good.size());
    if (!primed_)
    {
        if (fed_ >= n) return false;
        int16_t before[FRAME_SAMPLES];
        decoder_.decode(clip_.frames[fed_], clip_.good[fed_] != 0, before);
        fed_++;
        primed_ = true;
    }
    if (fed_ < n)
    {
        decoder_.decode(clip_.frames[fed_], clip_.good[fed_] != 0, frame_);
        frameAt_ = 0;
        fed_++;
        // After running dry, the first frame back fades in from the quiet:
        // no slot of the clip.
        if (resuming_)
            resuming_ = false;
        else
            slot_++;
        fading_ = 0;
        return true;
    }
    if (fed_ >= clip_.length)
    {
        if (flushed_) return false;
        decoder_.flush(frame_);
        frameAt_ = 0;
        flushed_ = true;
        slot_++;
        return true;
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
        if (frameAt_ < (size_t)FRAME_SAMPLES)
        {
            size_t take = std::min(n - given, (size_t)FRAME_SAMPLES - frameAt_);
            std::memcpy(out + given, frame_ + frameAt_, take * sizeof(int16_t));
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
        // Ran dry: wait for more, and come back in with a fade.
        state_ = State::Stalled;
        fading_ = 0;
        resuming_ = false;
        if (fadeStep())
        {
            resuming_ = true;
            continue;
        }
        resuming_ = primed_;
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
    primed_ = false;
    flushed_ = false;
    fading_ = 0;
    resuming_ = false;
    frameAt_ = FRAME_SAMPLES;
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
