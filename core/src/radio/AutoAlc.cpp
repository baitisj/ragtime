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
    version_++;
    // From here, never above the level it finds.
    if (on)
    {
        ceiling_ = level_;
        return std::nullopt;
    }
    return restore();
}

void AutoAlc::hostLevel(std::optional<double> dB)
{
    if (!dB)
    {
        if (level_) version_++;
        level_.reset();
        expected_.reset();
        return;
    }
    // A level we didn't send: the operator's own, or a restarted host's.
    bool ours = expected_ && same(*dB, *expected_);
    if (!ours) ceiling_ = *dB;
    expected_.reset();
    level_ = *dB;
    version_++;
}

void AutoAlc::operatorSet(double dB)
{
    ceiling_ = tenths(dB);
    expected_ = ceiling_;
    version_++;
}

std::optional<double> AutoAlc::burstEnded(const TxMeters::Burst& burst)
{
    if (!enabled_ || !level_ || !ceiling_ || burst.turnDownDb <= 0) return std::nullopt;
    double floor = std::max(LEVEL_MIN_DB, *ceiling_ - MAX_TRIM_DB);
    double target = tenths(std::max(floor, *level_ - burst.turnDownDb));
    if (target >= *level_ || same(target, *level_)) return std::nullopt;
    expected_ = target;
    version_++;
    return target;
}

std::optional<double> AutoAlc::restore()
{
    if (!level_ || !ceiling_ || *level_ >= *ceiling_ || same(*level_, *ceiling_)) return std::nullopt;
    expected_ = ceiling_;
    version_++;
    return ceiling_;
}

double AutoAlc::trimmedDb() const
{
    if (!level_ || !ceiling_) return 0;
    return std::max(0.0, *ceiling_ - *level_);
}

bool AutoAlc::atFloor() const
{
    if (!level_ || !ceiling_) return false;
    double floor = std::max(LEVEL_MIN_DB, *ceiling_ - MAX_TRIM_DB);
    return *level_ <= floor + 0.05;
}

} // namespace radio
} // namespace ragtime
