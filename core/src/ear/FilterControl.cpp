#include "ragtime/ear/FilterControl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ragtime/ear/Passbands.h"

namespace ragtime
{
namespace ear
{

namespace
{

// Hamlib's knob steps: the rig's 0 to 255.
double quantise(double knob)
{
    return std::round(std::max(0.0, std::min(1.0, knob)) * 255.0) / 255.0;
}

int tens(double hz)
{
    return (int)std::lround(hz / 10.0) * 10;
}

const char* const ARROW = " \xe2\x86\x92 ";

} // namespace

double knobShift(std::optional<double> knob, double hzPerKnob)
{
    return knob ? hzPerKnob * (*knob - 0.5) : 0.0;
}

Span spanOf(const RigFilter& f, double centreHz, double hzPerKnob)
{
    const double in = knobShift(f.pbtIn, hzPerKnob), out = knobShift(f.pbtOut, hzPerKnob);
    Span s;
    s.lowHz = centreHz - f.widthHz / 2.0 + std::max(in, out);
    s.highHz = centreHz + f.widthHz / 2.0 + std::min(in, out);
    // Shifted apart: nothing passes.
    if (s.lowHz > s.highHz) s.lowHz = s.highHz = (s.lowHz + s.highHz) / 2;
    return s;
}

double centreOf(const Span& measured, const RigFilter& f, double hzPerKnob)
{
    return measured.centre() - (knobShift(f.pbtIn, hzPerKnob) + knobShift(f.pbtOut, hzPerKnob)) / 2;
}

int roundWidth(double hz, bool up)
{
    hz = std::max(50.0, hz);
    const double step = hz <= 500 ? 50.0 : 100.0;
    const double n = up ? std::ceil(hz / step - 1e-9) : std::round(hz / step);
    return std::max(50, (int)(n * step));
}

RigFilter settingFor(const Span& wanted, const RigFilter& now, double centreHz, double hzPerKnob)
{
    RigFilter out = now;
    const double want = std::max(50.0, wanted.width());
    const bool knobs = hzPerKnob != 0 && now.pbtIn && now.pbtOut;
    if (!knobs)
    {
        // The width alone: the edges move together about the centre.
        out.widthHz = roundWidth(want, false);
        return out;
    }
    int width = now.widthHz > 0 ? now.widthHz : roundWidth(want, true);
    if (want > width) width = roundWidth(want, true);
    double in = 0.5, outKnob = 0.5;
    auto solve = [&](int w) {
        in = 0.5 + (wanted.lowHz - (centreHz - w / 2.0)) / hzPerKnob;
        outKnob = 0.5 + (wanted.highHz - (centreHz + w / 2.0)) / hzPerKnob;
        return in >= 0 && in <= 1 && outKnob >= 0 && outKnob <= 1;
    };
    // Past what the knobs reach: a narrower width, the knobs for the rest.
    if (!solve(width))
    {
        width = roundWidth(want, true);
        solve(width);
    }
    out.widthHz = width;
    out.pbtIn = quantise(in);
    out.pbtOut = quantise(outKnob);
    return out;
}

bool cutsData2G(const Span& span, bool narrow)
{
    const Passband p = data2gPassband(narrow);
    return span.lowHz > p.lowHz + 10 || span.highHz < p.highHz - 10;
}

std::string changeText(const Span& from, const Span& to)
{
    const int dl = tens(to.lowHz) - tens(from.lowHz), dh = tens(to.highHz) - tens(from.highHz);
    auto edge = [](const char* name, double a, double b) {
        return std::string(name) + " " + std::to_string(tens(a)) + ARROW + std::to_string(tens(b)) + " Hz";
    };
    if (dl == 0 && dh == 0) return "No change";
    if (dl == 0) return edge("High edge", from.highHz, to.highHz);
    if (dh == 0) return edge("Low edge", from.lowHz, to.lowHz);
    if (dl == dh) return "Shifted " + std::to_string(std::abs(dl)) + " Hz " + (dl > 0 ? "up" : "down");
    if (dl == -dh)
        return "Width " + std::to_string(tens(from.highHz) - tens(from.lowHz)) + ARROW +
               std::to_string(tens(to.highHz) - tens(to.lowHz)) + " Hz";
    return edge("Low edge", from.lowHz, to.lowHz) + ", " + edge("high edge", from.highHz, to.highHz);
}

void FilterEdit::seen(const RigFilter& f)
{
    if (!found_) found_ = f;
    current_ = f;
}

void FilterEdit::change(const RigFilter& to, uint64_t)
{
    if (!current_) return;
    // Undo goes back past every change since the last Keep.
    if (state_ == State::Idle || (state_ == State::Waiting && revert_ && !dirty_)) before_ = current_;
    pending_ = to;
    revert_ = false;
    state_ = State::Waiting;
}

void FilterEdit::keep()
{
    if (state_ != State::Trial) return;
    state_ = State::Idle;
    dirty_ = false;
    before_.reset();
    pending_.reset();
}

void FilterEdit::undo()
{
    if (state_ == State::Idle || (state_ == State::Waiting && revert_)) return;
    if (!dirty_)
    {
        // Nothing written yet: nothing to go back from.
        state_ = State::Idle;
        pending_.reset();
        before_.reset();
        return;
    }
    pending_ = before_;
    revert_ = true;
    state_ = State::Waiting;
}

void FilterEdit::putBack()
{
    if (!found_) return;
    pending_ = found_;
    revert_ = true;
    state_ = State::Waiting;
}

std::optional<RigFilter> FilterEdit::due(uint64_t nowMs, bool quiet)
{
    if (state_ == State::Trial && nowMs >= deadlineMs_) undo();
    if (state_ != State::Waiting || !quiet || !pending_) return std::nullopt;
    RigFilter write = *pending_;
    everWritten_ = true;
    if (revert_)
    {
        state_ = State::Idle;
        revert_ = false;
        dirty_ = false;
        before_.reset();
        pending_.reset();
    }
    else
    {
        state_ = State::Trial;
        dirty_ = true;
        deadlineMs_ = nowMs + KEEP_MS;
    }
    current_ = write;
    return write;
}

std::optional<RigFilter> FilterEdit::close()
{
    std::optional<RigFilter> write;
    if (state_ == State::Waiting && revert_)
        write = pending_;
    else if (dirty_)
        write = before_;
    if (write) everWritten_ = true;
    state_ = State::Idle;
    revert_ = dirty_ = false;
    before_.reset();
    pending_.reset();
    return write;
}

std::optional<RigFilter> FilterEdit::quit() const
{
    if (!everWritten_) return std::nullopt;
    return found_;
}

double FilterEdit::secondsLeft(uint64_t nowMs) const
{
    if (state_ != State::Trial || nowMs >= deadlineMs_) return 0;
    return (double)(deadlineMs_ - nowMs) / 1000.0;
}

bool Calibration::start(const RigFilter& now)
{
    if (!now.pbtIn || !now.pbtOut) return false;
    original_ = now;
    step_ = 0;
    spans_.clear();
    result_.reset();
    problem_.clear();
    return true;
}

void Calibration::cancel()
{
    step_ = -1;
    spans_.clear();
}

std::optional<RigFilter> Calibration::setting() const
{
    if (!running()) return std::nullopt;
    RigFilter f = original_;
    f.pbtIn = 0.5;
    f.pbtOut = 0.5;
    if (step_ == 1) f.pbtIn = quantise(0.5 + STEP);
    if (step_ == 2) f.pbtOut = quantise(0.5 - STEP);
    if (step_ == 3) f = original_;
    return f;
}

void Calibration::measured(const Span& span)
{
    if (!running()) return;
    if (step_ < 3) spans_.push_back(span);
    step_++;
    if (step_ != 3) return;
    // Each turn moved one filter: the low edge up, or the high edge down.
    const double stepIn = quantise(0.5 + STEP) - 0.5, stepOut = quantise(0.5 - STEP) - 0.5;
    const double movedIn = (spans_[1].lowHz - spans_[0].lowHz) + (spans_[1].highHz - spans_[0].highHz);
    const double movedOut = (spans_[2].lowHz - spans_[0].lowHz) + (spans_[2].highHz - spans_[0].highHz);
    const double kIn = movedIn / stepIn, kOut = movedOut / stepOut;
    char text[160];
    if (std::fabs(movedIn) < LEAST_HZ || std::fabs(movedOut) < LEAST_HZ)
    {
        std::snprintf(text, sizeof text,
                      "The edges hardly moved (%d Hz for PBT_IN, %d Hz for PBT_OUT): this rig's knobs may not be "
                      "the twin PBT, or the band is too quiet to see",
                      tens(movedIn), tens(movedOut));
        problem_ = text;
    }
    else if ((kIn > 0) != (kOut > 0) || std::fabs(kIn - kOut) > AGREE * std::max(std::fabs(kIn), std::fabs(kOut)))
    {
        std::snprintf(text, sizeof text, "The two knobs disagreed (%d and %d Hz for a full turn): try again on a "
                                         "steadier band",
                      tens(kIn), tens(kOut));
        problem_ = text;
    }
    else
    {
        result_ = (kIn + kOut) / 2;
    }
}

std::string Calibration::stepText() const
{
    switch (step_)
    {
    case 0: return "Measuring with the knobs centred (1 of 3)";
    case 1: return "Measuring with PBT_IN turned up (2 of 3)";
    case 2: return "Measuring with PBT_OUT turned down (3 of 3)";
    case 3: return "Putting the knobs back";
    default: break;
    }
    return std::string();
}

} // namespace ear
} // namespace ragtime
