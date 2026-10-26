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
    // From here, never above the level it finds.
    if (on)
    {
        if (newest()) ceiling_ = newest();
        return std::nullopt;
    }
    return restore();
}

std::optional<double> AutoAlc::newest() const
{
    if (!sent_.empty()) return sent_.back();
    return level_;
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
    // new ceiling, and what was sent before it is moot.
    if (!sent_.empty() && same(*dB, sent_.front()))
    {
        sent_.pop_front();
    }
    else
    {
        ceiling_ = *dB;
        sent_.clear();
        setMs_ = nowMs;
        wantsMore_ = false;
    }
    level_ = *dB;
    version_++;
}

void AutoAlc::operatorSet(double dB, uint64_t nowMs)
{
    ceiling_ = tenths(dB);
    setMs_ = nowMs;
    wantsMore_ = false;
    version_++;
}

std::optional<double> AutoAlc::burstEnded(const TxMeters::Burst& burst)
{
    std::optional<double> now = newest();
    if (!enabled_ || held_ || !now || !ceiling_ || burst.alcStepDb == 0) return std::nullopt;
    // Sent at the level before the operator's: it says nothing of this one.
    if (burst.startMs < setMs_) return std::nullopt;
    double floor = std::max(LEVEL_MIN_DB, *ceiling_ - MAX_TRIM_DB);
    double target = tenths(std::min(*ceiling_, std::max(floor, *now + burst.alcStepDb)));
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
    if (!now || !ceiling_ || same(*now, *ceiling_)) return std::nullopt;
    return ceiling_;
}

void AutoAlc::sent(double dB)
{
    sent_.push_back(tenths(dB));
    version_++;
}

double AutoAlc::trimmedDb() const
{
    std::optional<double> now = newest();
    if (!now || !ceiling_) return 0;
    return std::max(0.0, *ceiling_ - *now);
}

bool AutoAlc::atFloor() const
{
    std::optional<double> now = newest();
    if (!now || !ceiling_) return false;
    double floor = std::max(LEVEL_MIN_DB, *ceiling_ - MAX_TRIM_DB);
    return *now <= floor + 0.05;
}

} // namespace radio
} // namespace ragtime
