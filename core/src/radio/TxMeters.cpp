#include "ragtime/radio/TxMeters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ragtime
{
namespace radio
{

namespace
{

std::string percent(double share)
{
    return std::to_string((int)std::lround(std::max(0.0, share) * 100)) + "%";
}

std::string fixed(double value, int places)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", places, value);
    return text;
}

std::string watts(double w)
{
    return fixed(w, w < 10 ? 1 : 0) + " W";
}

const std::string DOT = " \xc2\xb7 ";

void join(std::string& text, const std::string& part, const std::string& between = DOT)
{
    text += (text.empty() ? "" : between) + part;
}

} // namespace

void TxMeters::transmitting(bool on, uint64_t nowMs)
{
    if (on == on_) return;
    on_ = on;
    if (on)
    {
        current_ = Burst();
        current_.startMs = nowMs;
        alcSum_ = wattsSum_ = powerSum_ = 0;
        alcCount_ = wattsCount_ = powerCount_ = 0;
        overInARow_ = 0;
    }
    else
    {
        current_.endMs = nowMs;
        if (current_.readings > 0)
        {
            last_ = current_;
            bursts_++;
        }
        latest_ = MeterReading();
    }
    version_++;
}

void TxMeters::reading(const MeterReading& reading, uint64_t nowMs)
{
    if (!on_ || !reading.any()) return;
    current_.readings++;
    current_.endMs = nowMs;
    if (reading.alc)
    {
        double alc = *reading.alc;
        latest_.alc = alc;
        current_.alcPeak = std::max(alc, current_.alcPeak.value_or(alc));
        alcSum_ += alc;
        current_.alcMean = alcSum_ / ++alcCount_;
        // Glissando's Drive knob: one reading far over, or two in a row
        // over, and down it goes; then it counts afresh.
        if (alc > ALC_HIGH)
        {
            current_.turnDownDb += BIG_STEP_DB;
            overInARow_ = 0;
        }
        else if (alc > ALC_TARGET && ++overInARow_ >= 2)
        {
            current_.turnDownDb += STEP_DB;
            overInARow_ = 0;
        }
        else if (alc <= ALC_TARGET)
        {
            overInARow_ = 0;
        }
    }
    if (reading.swr)
    {
        latest_.swr = *reading.swr;
        current_.swrPeak = std::max(*reading.swr, current_.swrPeak.value_or(*reading.swr));
    }
    if (reading.watts)
    {
        latest_.watts = *reading.watts;
        wattsSum_ += *reading.watts;
        current_.wattsMean = wattsSum_ / ++wattsCount_;
    }
    if (reading.power)
    {
        latest_.power = *reading.power;
        powerSum_ += *reading.power;
        current_.powerMean = powerSum_ / ++powerCount_;
    }
    version_++;
}

std::string readingText(const MeterReading& reading)
{
    std::string text;
    if (reading.alc) join(text, "ALC " + percent(*reading.alc));
    if (reading.swr) join(text, "SWR " + fixed(*reading.swr, 1));
    if (reading.watts)
        join(text, watts(*reading.watts));
    else if (reading.power)
        join(text, percent(*reading.power) + " power");
    return text;
}

std::string burstText(const TxMeters::Burst& burst, const std::string& between)
{
    uint64_t ms = burst.endMs > burst.startMs ? burst.endMs - burst.startMs : 0;
    std::string text;
    if (burst.alcPeak)
        join(text, "ALC peak " + percent(*burst.alcPeak) + ", mean " + percent(*burst.alcMean), between);
    if (burst.swrPeak) join(text, "SWR " + fixed(*burst.swrPeak, 1), between);
    if (burst.wattsMean)
        join(text, watts(*burst.wattsMean), between);
    else if (burst.powerMean)
        join(text, percent(*burst.powerMean) + " power", between);
    return fixed(ms / 1000.0, ms < 10000 ? 1 : 0) + " s" + (text.empty() ? std::string() : ": " + text);
}

} // namespace radio
} // namespace ragtime
