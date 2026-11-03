#include "ragtime/voice/PlayWatch.h"

namespace ragtime
{
namespace voice
{

void PlayWatch::start(uint64_t nowMs, uint64_t total)
{
    running_ = true;
    total_ = total;
    known_ = total > 0;
    taken_ = 0;
    movedMs_ = nowMs;
    allTakenMs_ = 0;
}

void PlayWatch::ended(uint64_t total)
{
    if (!running_ || known_) return;
    total_ = total;
    known_ = true;
}

PlayWatch::Verdict PlayWatch::check(uint64_t nowMs, uint64_t taken, uint64_t ready)
{
    if (!running_) return Verdict::Playing;
    if (taken > taken_)
    {
        taken_ = taken;
        movedMs_ = nowMs;
    }
    if (known_ && taken_ >= total_)
    {
        if (allTakenMs_ == 0) allTakenMs_ = nowMs == 0 ? 1 : nowMs;
        if (nowMs >= allTakenMs_ + END_GRACE_MS)
        {
            running_ = false;
            return Verdict::Ended;
        }
        return Verdict::Playing;
    }
    // Sound was there to take and the speaker took none of it. A clip
    // still coming that has run dry hands nothing over, and isn't stalled.
    if (ready > taken_ && nowMs >= movedMs_ + STALL_MS)
    {
        running_ = false;
        return Verdict::Stalled;
    }
    return Verdict::Playing;
}

} // namespace voice
} // namespace ragtime
