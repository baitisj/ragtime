// The deck's knobs, after Glissando's Dial: see Knob.h.
#include "ragtime/deck/Knob.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ragtime
{
namespace deck
{

Knob::Knob(const KnobSpec& spec) : spec_(spec), value_(0.0), pushed_(spec.defaultPushed)
{
    if (spec_.maximum < spec_.minimum) std::swap(spec_.maximum, spec_.minimum);
    if (!(spec_.step > 0.0)) spec_.step = (spec_.maximum - spec_.minimum) / 100.0;
    if (!spec_.positions.empty())
    {
        std::sort(spec_.positions.begin(), spec_.positions.end());
        spec_.minimum = spec_.positions.front();
        spec_.maximum = spec_.positions.back();
    }
    spec_.defaultValue = snap(spec_.defaultValue);
    value_ = spec_.defaultValue;
}

int Knob::positionOf(double value) const
{
    int best = 0;
    for (size_t i = 1; i < spec_.positions.size(); i++)
        if (std::fabs(spec_.positions[i] - value) < std::fabs(spec_.positions[best] - value)) best = (int)i;
    return best;
}

double Knob::snap(double value) const
{
    if (!spec_.positions.empty()) return spec_.positions[positionOf(value)];
    value = std::min(spec_.maximum, std::max(spec_.minimum, value));
    value = std::round((value - spec_.minimum) / spec_.step) * spec_.step + spec_.minimum;
    value = std::min(spec_.maximum, std::max(spec_.minimum, value));
    // Kept to the step's own decimals, so 0.1 steps don't drift to
    // -25.999999.
    value = std::round(value * 1e6) / 1e6;
    if (value == 0.0) value = 0.0; // never "-0.0"
    return value;
}

double Knob::fractionOf(double value) const
{
    if (!spec_.positions.empty())
    {
        if (spec_.positions.size() < 2) return 0.5;
        return (double)positionOf(value) / (double)(spec_.positions.size() - 1);
    }
    double span = std::max(1e-9, spec_.maximum - spec_.minimum);
    double f;
    if (std::isnan(spec_.centre))
        f = (value - spec_.minimum) / span;
    else if (value <= spec_.centre)
        f = 0.5 * (value - spec_.minimum) / std::max(1e-9, spec_.centre - spec_.minimum);
    else
        f = 0.5 + 0.5 * (value - spec_.centre) / std::max(1e-9, spec_.maximum - spec_.centre);
    return std::min(1.0, std::max(0.0, f));
}

double Knob::valueAt(double fraction) const
{
    fraction = std::min(1.0, std::max(0.0, fraction));
    if (!spec_.positions.empty())
    {
        if (spec_.positions.size() < 2) return spec_.positions.front();
        long i = std::lround(fraction * (double)(spec_.positions.size() - 1));
        return spec_.positions[(size_t)i];
    }
    if (std::isnan(spec_.centre)) return spec_.minimum + fraction * (spec_.maximum - spec_.minimum);
    if (fraction <= 0.5) return spec_.minimum + fraction * 2.0 * (spec_.centre - spec_.minimum);
    return spec_.centre + (fraction - 0.5) * 2.0 * (spec_.maximum - spec_.centre);
}

bool Knob::change(double value)
{
    value = snap(value);
    if (value == value_) return false;
    value_ = value;
    return true;
}

bool Knob::setValue(double value)
{
    return change(value);
}

bool Knob::setPushed(bool pushed)
{
    if (pushed_ == pushed) return false;
    pushed_ = pushed;
    return true;
}

void Knob::press(int y)
{
    dragging_ = true;
    turned_ = false;
    dragY_ = y;
    dragValue_ = value_;
}

bool Knob::drag(int y, bool fine)
{
    if (!dragging_) return false;
    double perPixel = 1.0 / KNOB_SWEEP_PIXELS * (fine ? 0.1 : 1.0);
    double fraction = fractionOf(dragValue_) + (dragY_ - y) * perPixel;
    fraction = std::min(1.0, std::max(0.0, fraction));
    double value = valueAt(fraction);
    // Catch on the default for a few pixels either side, so it can be
    // found again by hand.
    if (spec_.positions.empty()
        && std::fabs(fraction - fractionOf(spec_.defaultValue)) <= KNOB_DETENT_PIXELS * perPixel)
        value = spec_.defaultValue;
    // A push-pull knob has to move a few pixels before it turns, so a
    // click that wobbles is still a push.
    if (!spec_.pushable || std::abs(y - dragY_) >= KNOB_PUSH_SLOP_PIXELS) turned_ = true;
    return turned_ && change(value);
}

bool Knob::release(int y)
{
    bool clicked = dragging_ && !turned_ && std::abs(y - dragY_) < KNOB_PUSH_SLOP_PIXELS;
    dragging_ = false;
    if (swallowRelease_)
    {
        swallowRelease_ = false;
        return false;
    }
    if (!spec_.pushable || !clicked) return false;
    pushed_ = !pushed_;
    return true;
}

double Knob::wheelStep(bool shift, bool control) const
{
    if (spec_.wheelPlain > 0.0)
    {
        if (control && spec_.wheelControl > 0.0) return spec_.wheelControl;
        if (shift && spec_.wheelShift > 0.0) return spec_.wheelShift;
        return spec_.wheelPlain;
    }
    return shift ? spec_.step : std::max(spec_.step, (spec_.maximum - spec_.minimum) / 100.0);
}

bool Knob::wheel(int rotation, int delta, bool shift, bool control)
{
    // Smooth-scrolling wheels and touchpads send part clicks: save them up.
    delta = std::max(1, delta);
    wheelRotation_ += rotation;
    int clicks = wheelRotation_ / delta;
    wheelRotation_ -= clicks * delta;
    if (clicks == 0) return false;
    if (!spec_.positions.empty())
    {
        int i = positionOf(value_) + clicks;
        i = std::min((int)spec_.positions.size() - 1, std::max(0, i));
        return change(spec_.positions[(size_t)i]);
    }
    return change(value_ + clicks * wheelStep(shift, control));
}

bool Knob::doubleClick()
{
    bool changed = change(spec_.defaultValue);
    if (spec_.pushable)
    {
        changed = setPushed(spec_.defaultPushed) || changed;
        // The release that follows a double click is no push.
        swallowRelease_ = true;
        dragging_ = false;
    }
    return changed;
}

bool Knob::key(KnobKey key, bool shift)
{
    if (key == KnobKey::Push)
    {
        if (!spec_.pushable) return false;
        pushed_ = !pushed_;
        return true;
    }
    if (!spec_.positions.empty())
    {
        int i = positionOf(value_);
        int last = (int)spec_.positions.size() - 1;
        switch (key)
        {
        case KnobKey::Up:
        case KnobKey::Right:
        case KnobKey::PageUp: i++; break;
        case KnobKey::Down:
        case KnobKey::Left:
        case KnobKey::PageDown: i--; break;
        case KnobKey::Home: i = 0; break;
        case KnobKey::End: i = last; break;
        case KnobKey::Push: break;
        }
        return change(spec_.positions[(size_t)std::min(last, std::max(0, i))]);
    }
    switch (key)
    {
    case KnobKey::Up:
    case KnobKey::Right: return change(value_ + wheelStep(shift, false));
    case KnobKey::Down:
    case KnobKey::Left: return change(value_ - wheelStep(shift, false));
    case KnobKey::PageUp: return change(value_ + wheelStep(false, true));
    case KnobKey::PageDown: return change(value_ - wheelStep(false, true));
    case KnobKey::Home: return change(spec_.minimum);
    case KnobKey::End: return change(spec_.maximum);
    case KnobKey::Push: break;
    }
    return false;
}

} // namespace deck
} // namespace ragtime
