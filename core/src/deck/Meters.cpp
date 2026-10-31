// The deck's meters and push to talk: see Meters.h.
#include "ragtime/deck/Meters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ragtime
{
namespace deck
{

double faceFraction(double dbfs, double floor, double top)
{
    if (!(dbfs > floor)) return 0.0;
    return std::min(1.0, (dbfs - floor) / std::max(1e-9, top - floor));
}

void VuNeedle::step(double dbfs, double seconds)
{
    double target = std::isfinite(dbfs) ? std::max(floor_, dbfs) : floor_;
    if (!(seconds > 0.0)) return;
    double k = 1.0 - std::exp(-seconds / std::max(1e-3, tau_));
    position_ += (target - position_) * k;
}

void PeakNeedle::step(double dbfs, double seconds)
{
    double reading = std::isfinite(dbfs) ? std::max(floor_, dbfs) : floor_;
    seconds = std::max(0.0, seconds);
    // The needle jumps up at once and falls back steadily.
    needle_ = std::max(reading, std::max(floor_, needle_ - NEEDLE_FALL_DB_PER_S * seconds));
    // The hold mark: up at once, then a second's wait, then a slow fall.
    if (reading >= hold_)
    {
        hold_ = reading;
        held_ = 0.0;
        return;
    }
    held_ += seconds;
    if (held_ > HOLD_WAIT_S)
    {
        double falling = std::min(seconds, held_ - HOLD_WAIT_S);
        hold_ = std::max(std::max(reading, floor_), hold_ - HOLD_FALL_DB_PER_S * falling);
    }
}

void PeakNeedle::reset()
{
    needle_ = floor_;
    hold_ = floor_;
    held_ = 0.0;
}

std::string dbText(double dbfs, double floor)
{
    if (!std::isfinite(dbfs) || dbfs <= floor) return "\xE2\x80\x94"; // an em dash
    long n = std::lround(dbfs);
    char buf[32];
    if (n < 0)
        std::snprintf(buf, sizeof buf, "\xE2\x88\x92%ld", -n);  // a true minus sign
    else
        std::snprintf(buf, sizeof buf, "%ld", n);
    return buf;
}

//-------------------------------------------------------------------------
// Talk
//-------------------------------------------------------------------------

bool Talk::press()
{
    if (phase_ == TalkPhase::Recording || phase_ == TalkPhase::Coding) return false;
    bool wasMonitoring = monitoring_;
    monitoring_ = false;
    phase_ = TalkPhase::Recording;
    return wasMonitoring;
}

bool Talk::release()
{
    if (phase_ != TalkPhase::Recording) return false;
    phase_ = TalkPhase::Coding;
    return true;
}

bool Talk::discard()
{
    if (phase_ != TalkPhase::Recording) return false;
    phase_ = TalkPhase::Idle;
    return true;
}

void Talk::coded(bool holdForReview, bool empty)
{
    if (phase_ != TalkPhase::Coding) return;
    phase_ = holdForReview && !empty ? TalkPhase::Review : TalkPhase::Idle;
}

bool Talk::sendReviewed()
{
    if (phase_ != TalkPhase::Review) return false;
    phase_ = TalkPhase::Idle;
    return true;
}

bool Talk::discardReviewed()
{
    return sendReviewed();
}

bool Talk::startMonitor(bool haveClip)
{
    if (!canMonitor(haveClip)) return false;
    monitoring_ = true;
    return true;
}

//-------------------------------------------------------------------------
// The airtime bar
//-------------------------------------------------------------------------

double AirBar::scaleSeconds() const
{
    double mark = std::max(1.0, maxSeconds);
    if (airSeconds <= mark) return mark;
    // Past the mark the scale grows a quarter at a time, so the bar never
    // pins at its end.
    double scale = mark;
    while (scale < airSeconds) scale *= 1.25;
    return scale;
}

std::string clockText(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    long s = std::lround(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
    return buf;
}

std::string AirBar::words() const
{
    return clockText(airSeconds) + " of air for " + clockText(speechSeconds) + " of speech";
}

} // namespace deck
} // namespace ragtime
