#include "ragtime/radio/Noise.h"

#include <algorithm>
#include <cmath>

namespace ragtime
{
namespace radio
{

double sUnits(double dbOverS9)
{
    return 9.0 + dbOverS9 / DB_PER_S_UNIT;
}

std::string sUnitText(double dbOverS9)
{
    if (dbOverS9 >= 2.5)
        return "S9+" + std::to_string((int)(std::lround(dbOverS9 / 5.0) * 5));
    long s = std::lround(sUnits(dbOverS9));
    return "S" + std::to_string(std::max(0L, std::min(9L, s)));
}

std::string levelText(double dbOverS9)
{
    long dB = std::lround(dbOverS9);
    return (dB > 0 ? "+" : "") + std::to_string(dB);
}

void NoiseFloor::reading(double dbOverS9, uint64_t nowMs)
{
    readings_.emplace_back(nowMs, dbOverS9);
    while (!readings_.empty() && readings_.front().first + WINDOW_MS < nowMs) readings_.pop_front();
    version_++;
}

void NoiseFloor::clear()
{
    readings_.clear();
    version_++;
}

std::optional<double> NoiseFloor::dbOverS9(uint64_t nowMs) const
{
    if (readings_.empty() || readings_.back().first + STALE_MS < nowMs) return std::nullopt;
    std::optional<double> quietest;
    for (const auto& [at, dB] : readings_)
    {
        if (at + WINDOW_MS < nowMs) continue;
        if (!quietest || dB < *quietest) quietest = dB;
    }
    return quietest;
}

} // namespace radio
} // namespace ragtime
