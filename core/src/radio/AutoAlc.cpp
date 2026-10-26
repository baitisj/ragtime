#include "ragtime/radio/AutoAlc.h"

#include <algorithm>
#include <cmath>

namespace ragtime
{
namespace radio
{

namespace
{

// TXPOWER answers to a tenth of a dB.
double tenths(double dB)
{
    return std::round(dB * 10.0) / 10.0;
}

bool same(double a, double b)
{
    return std::fabs(a - b) < 0.05;
}

} // namespace

std::optional<double> AutoAlc::setEnabled(bool on)
{
    if (on == enabled_) return std::nullopt;
    enabled_ = on;
    wantsMore_ = false;
    version_++;
    // From here, around the level it finds.
    if (on)
    {
        if (newest()) base_ = newest();
        return std::nullopt;
    }
    return restore();
}

std::optional<double> AutoAlc::newest() const
{
    if (!sent_.empty()) return sent_.back();
    return level_;
}

std::optional<double> AutoAlc::top() const
{
    if (!base_) return std::nullopt;
    return std::min(LEVEL_MAX_DB, *base_ + CLIMB_DB);
}

std::optional<double> AutoAlc::floor() const
{
    if (!base_) return std::nullopt;
    return std::max(LEVEL_MIN_DB, *base_ - MAX_TRIM_DB);
}

void AutoAlc::hostLevel(std::optional<double> dB, uint64_t nowMs)
{
    if (!dB)
    {
        if (level_) version_++;
        level_.reset();
        sent_.clear();
        return;
    }
    // Each TXPOWER we send is answered in turn. A level we didn't send is
    // the operator's own (data2g-host's window) or a restarted host's: the
    // new base, and what was sent before it is moot.
    if (!sent_.empty() && same(*dB, sent_.front()))
    {
        sent_.pop_front();
    }
    else
    {
        base_ = *dB;
        sent_.clear();
        setMs_ = nowMs;
        wantsMore_ = false;
    }
    level_ = *dB;
    version_++;
}

void AutoAlc::operatorSet(double dB, uint64_t nowMs)
{
    base_ = tenths(dB);
    setMs_ = nowMs;
    wantsMore_ = false;
    version_++;
}

void AutoAlc::sent(double dB)
{
    sent_.push_back(tenths(dB));
    version_++;
}

std::optional<double> AutoAlc::burstEnded(const TxMeters::Burst& burst)
{
    std::optional<double> now = newest();
    if (!enabled_ || held_ || !now || !base_ || burst.alcStepDb == 0) return std::nullopt;
    // Sent at the level before the operator's: it says nothing of this one.
    if (burst.startMs < setMs_) return std::nullopt;
    double target = tenths(std::min(*top(), std::max(*floor(), *now + burst.alcStepDb)));
    bool more = burst.alcStepDb > 0 && target <= *now + 0.05;
    if (more != wantsMore_)
    {
        wantsMore_ = more;
        version_++;
    }
    if (same(target, *now)) return std::nullopt;
    return target;
}

std::optional<double> AutoAlc::restore()
{
    std::optional<double> now = newest();
    if (!now || !base_ || same(*now, *base_)) return std::nullopt;
    return base_;
}

double AutoAlc::offsetDb() const
{
    std::optional<double> now = newest();
    if (!now || !base_) return 0;
    return *now - *base_;
}

bool AutoAlc::atFloor() const
{
    std::optional<double> now = newest();
    if (!now || !base_) return false;
    return *now <= *floor() + 0.05;
}

} // namespace radio
} // namespace ragtime
