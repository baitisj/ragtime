#include "ragtime/ear/FilterControl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

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

double NotchModel::rawFor(double hz) const
{
    return quantise(hzPerUnit != 0 ? (hz - offsetHz) / hzPerUnit : 0.5);
}

void NotchModel::learn(double raw, double hz)
{
    if (seenRaw && seenHz && std::fabs(raw - *seenRaw) >= SLOPE_SPAN)
    {
        // Two dips: the slope too, if it is a believable one.
        const double slope = (hz - *seenHz) / (raw - *seenRaw);
        if (std::fabs(slope) >= 1000 && std::fabs(slope) <= 20000) hzPerUnit = slope;
    }
    offsetHz = hz - hzPerUnit * raw;
    seenRaw = raw;
    seenHz = hz;
}

std::string NotchModel::text() const
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(1);
    out << hzPerUnit << ' ' << offsetHz << ' ';
    if (seenRaw && seenHz)
    {
        out.precision(4);
        out << *seenRaw << ' ';
        out.precision(1);
        out << *seenHz;
    }
    else
    {
        out << "- -";
    }
    return out.str();
}

std::optional<NotchModel> NotchModel::fromText(const std::string& text)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    NotchModel m;
    std::string raw, hz;
    if (!(in >> m.hzPerUnit >> m.offsetHz >> raw >> hz) || std::fabs(m.hzPerUnit) < 1) return std::nullopt;
    if (raw != "-" && hz != "-")
    {
        std::istringstream r(raw + " " + hz);
        r.imbue(std::locale::classic());
        double a = 0, b = 0;
        if (r >> a >> b)
        {
            m.seenRaw = a;
            m.seenHz = b;
        }
    }
    return m;
}

bool notchCutsData2G(double hz, bool narrow)
{
    const Passband p = data2gPassband(narrow);
    return hz + NotchModel::WIDTH_HZ / 2 > p.lowHz && hz - NotchModel::WIDTH_HZ / 2 < p.highHz;
}

std::string notchChangeText(const RigFilter& from, const RigFilter& to, const NotchModel& m)
{
    const bool was = from.notchOn && *from.notchOn, is = to.notchOn && *to.notchOn;
    if (!was && !is) return std::string();
    if (!is) return "Notch off";
    const int at = to.notchRaw ? tens(m.hzOf(*to.notchRaw)) : 0;
    if (!was) return "Notch on at " + std::to_string(at) + " Hz";
    const int before = from.notchRaw ? tens(m.hzOf(*from.notchRaw)) : 0;
    if (before == at) return std::string();
    return "Notch " + std::to_string(before) + ARROW + std::to_string(at) + " Hz";
}

RigFilter withNotch(const RigFilter& now, std::optional<double> hz, const NotchModel& m)
{
    RigFilter out = now;
    out.notchOn = hz.has_value();
    if (hz) out.notchRaw = m.rawFor(*hz);
    return out;
}

double knobShift(std::optional<double> knob, double hzPerKnob)
{
    return knob ? hzPerKnob * (*knob - 0.5) : 0.0;
}

KnobModel KnobModel::about(double centreHz, const RigFilter& f)
{
    KnobModel m;
    m.centreHz = centreHz;
    m.widthHz = m.nominalHz = f.widthHz;
    return m;
}

std::string KnobModel::text() const
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(1);
    out << hzPerKnob << ' ' << centreHz << ' ' << widthHz << ' ' << nominalHz << ' ' << floorHz << ' ' << ceilingHz;
    return out.str();
}

std::optional<KnobModel> KnobModel::fromText(const std::string& text)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    KnobModel m;
    if (!(in >> m.hzPerKnob >> m.centreHz >> m.widthHz >> m.nominalHz >> m.floorHz >> m.ceilingHz)) return std::nullopt;
    if (!m.known() || m.widthHz <= 0 || m.nominalHz <= 0) return std::nullopt;
    return m;
}

Span spanOf(const RigFilter& f, const KnobModel& m)
{
    const double in = knobShift(f.pbtIn, m.hzPerKnob), out = knobShift(f.pbtOut, m.hzPerKnob);
    // The real width follows the rig's, as it was when measured.
    const double width = f.widthHz + (m.widthHz - m.nominalHz);
    Span s;
    s.lowHz = m.centreHz - width / 2.0 + std::max(in, out);
    s.highHz = m.centreHz + width / 2.0 + std::min(in, out);
    // Shifted apart: nothing passes.
    if (s.lowHz > s.highHz) s.lowHz = s.highHz = (s.lowHz + s.highHz) / 2;
    return s;
}

Span visible(const Span& s, const KnobModel& m)
{
    Span v = s;
    if (m.floorHz > 0) v.lowHz = std::max(v.lowHz, m.floorHz);
    if (m.ceilingHz > 0) v.highHz = std::min(v.highHz, m.ceilingHz);
    if (v.lowHz > v.highHz) v.lowHz = v.highHz;
    return v;
}

Span spanOf(const RigFilter& f, double centreHz, double hzPerKnob)
{
    KnobModel m = KnobModel::about(centreHz, f);
    m.hzPerKnob = hzPerKnob;
    return spanOf(f, m);
}

Span dragTarget(const Span& from, const Span& to, const RigFilter& now, const KnobModel& m)
{
    const Span real = spanOf(now, m);
    Span out{real.lowHz + (to.lowHz - from.lowHz), real.highHz + (to.highHz - from.highHz)};
    // A hidden edge dragged by itself: where it was dropped. In a shift
    // both edges move alike, hidden or not.
    const double dl = to.lowHz - from.lowHz, dh = to.highHz - from.highHz;
    if (std::fabs(dl - dh) < 1) return out;
    if (m.floorHz > 0 && real.lowHz < m.floorHz && std::fabs(dl) >= 1) out.lowHz = to.lowHz;
    if (m.ceilingHz > 0 && real.highHz > m.ceilingHz && std::fabs(dh) >= 1) out.highHz = to.highHz;
    return out;
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

RigFilter settingFor(const Span& wanted, const RigFilter& now, const KnobModel& m)
{
    RigFilter out = now;
    const double want = std::max(50.0, wanted.width());
    // The rig's widths are nominal: what it passes is this much more.
    const double extra = m.widthHz - m.nominalHz;
    const bool knobs = m.known() && now.pbtIn && now.pbtOut;
    if (!knobs)
    {
        // The width alone: the edges move together about the centre.
        out.widthHz = roundWidth(want - extra, false);
        return out;
    }
    int width = now.widthHz > 0 ? now.widthHz : roundWidth(want - extra, true);
    if (want > width + extra) width = roundWidth(want - extra, true);
    double in = 0.5, outKnob = 0.5;
    const double k = m.hzPerKnob;
    auto solve = [&](int w) {
        const double half = (w + extra) / 2.0;
        in = 0.5 + (wanted.lowHz - (m.centreHz - half)) / k;
        outKnob = 0.5 + (wanted.highHz - (m.centreHz + half)) / k;
        return in >= 0 && in <= 1 && outKnob >= 0 && outKnob <= 1;
    };
    // Past what the knobs reach: a narrower width, the knobs for the rest.
    if (!solve(width))
    {
        width = roundWidth(want - extra, true);
        solve(width);
    }
    out.widthHz = width;
    out.pbtIn = quantise(in);
    out.pbtOut = quantise(outKnob);
    return out;
}

RigFilter settingFor(const Span& wanted, const RigFilter& now, double centreHz, double hzPerKnob)
{
    KnobModel m = KnobModel::about(centreHz, now);
    m.hzPerKnob = hzPerKnob;
    return settingFor(wanted, now, m);
}

RigFilter shiftedBy(const RigFilter& now, double hz, double hzPerKnob)
{
    RigFilter out = now;
    if (!now.pbtIn || !now.pbtOut || hzPerKnob <= 0) return out;
    const double in = *now.pbtIn, o = *now.pbtOut;
    const double turn = std::max(-std::min(in, o), std::min(1 - std::max(in, o), hz / hzPerKnob));
    out.pbtIn = quantise(in + turn);
    out.pbtOut = quantise(o + turn);
    return out;
}

double guessHzPerKnob(const RigFilter& f)
{
    return f.widthHz > 0 ? f.widthHz : 3000;
}

RigFilter disabled(const RigFilter& now)
{
    RigFilter out = now;
    if (out.pbtIn) out.pbtIn = quantise(0.5);
    if (out.pbtOut) out.pbtOut = quantise(0.5);
    if (out.notchOn) out.notchOn = false;
    return out;
}

bool filtersOff(const RigFilter& f)
{
    return (!f.pbtIn || radio::pbtCentred(*f.pbtIn)) && (!f.pbtOut || radio::pbtCentred(*f.pbtOut)) &&
           !(f.notchOn && *f.notchOn);
}

bool holds(const RigFilter& rig, const RigFilter& preset)
{
    auto near = [](const std::optional<double>& a, const std::optional<double>& b) {
        return !b || (a && std::fabs(*a - *b) <= 1.5 / 255.0);
    };
    if (rig.mode != preset.mode || rig.widthHz != preset.widthHz) return false;
    if (!near(rig.pbtIn, preset.pbtIn) || !near(rig.pbtOut, preset.pbtOut)) return false;
    if (!preset.notchOn) return true;
    const bool on = rig.notchOn && *rig.notchOn;
    if (on != *preset.notchOn) return false;
    return !on || near(rig.notchRaw, preset.notchRaw);
}

bool cutsData2G(const Span& span, bool narrow)
{
    const Passband p = data2gPassband(narrow);
    return span.lowHz > p.lowHz + 10 || span.highHz < p.highHz - 10;
}

std::string changeText(const Span& from, const Span& to)
{
    const int dl = tens(to.lowHz - from.lowHz), dh = tens(to.highHz - from.highHz);
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
    // Ragtime never changes the mode, so the operator did: what it had of
    // the last mode is moot, and writing it would change the mode back.
    if (current_ && f.mode != current_->mode) *this = FilterEdit();
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
    if (state_ == State::Trial && !held_ && nowMs >= deadlineMs_) undo();
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
        heldLeftMs_ = KEEP_MS;
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
    if (state_ != State::Trial) return 0;
    if (held_) return (double)heldLeftMs_ / 1000.0;
    if (nowMs >= deadlineMs_) return 0;
    return (double)(deadlineMs_ - nowMs) / 1000.0;
}

void FilterEdit::hold(bool held, uint64_t nowMs)
{
    if (held == held_) return;
    held_ = held;
    if (held)
        heldLeftMs_ = deadlineMs_ > nowMs ? deadlineMs_ - nowMs : 0;
    else
        deadlineMs_ = nowMs + heldLeftMs_;
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
    if (step_ == PUT_BACK) return original_;
    RigFilter f = original_;
    f.pbtIn = 0.5;
    f.pbtOut = 0.5;
    if (step_ == 1) f.pbtIn = quantise(0.5 + STEP);
    if (step_ == 2) f.pbtIn = quantise(0.5 + 2 * STEP);
    if (step_ == 3) f.pbtOut = quantise(0.5 - STEP);
    if (step_ == 4) f.pbtOut = quantise(0.5 - 2 * STEP);
    return f;
}

void Calibration::measured(const Span& span)
{
    if (!running()) return;
    if (step_ < MEASURES) spans_.push_back(span);
    step_++;
    if (step_ != PUT_BACK) return;
    const Span& centred = spans_[0];
    // The knobs' turns, as the rig takes them.
    const double up1 = quantise(0.5 + STEP) - 0.5, up2 = quantise(0.5 + 2 * STEP) - 0.5;
    const double down1 = quantise(0.5 - STEP) - 0.5, down2 = quantise(0.5 - 2 * STEP) - 0.5;
    // Each pair moved one edge; between its two turns, both in sight.
    auto moved = [](const Span& a, const Span& b) { return (b.lowHz - a.lowHz) + (b.highHz - a.highHz); };
    const double movedUp = moved(spans_[1], spans_[2]);
    const double movedDown = moved(spans_[3], spans_[4]);
    const double kUp = movedUp / (up2 - up1), kDown = movedDown / (down2 - down1);
    char text[240];
    if (std::fabs(movedUp) < LEAST_HZ || std::fabs(movedDown) < LEAST_HZ)
    {
        std::snprintf(text, sizeof text,
                      "The edges hardly moved (%d Hz for a turn up, %d Hz for a turn down): this rig's knobs may "
                      "not be the twin PBT, or the band is too quiet to see",
                      tens(movedUp), tens(movedDown));
        problem_ = text;
        return;
    }
    if ((kUp > 0) != (kDown > 0) || std::fabs(kUp - kDown) > AGREE * std::max(std::fabs(kUp), std::fabs(kDown)))
    {
        std::snprintf(text, sizeof text, "The two knobs disagreed (%d and %d Hz for a full turn): try again on a "
                                         "steadier band",
                      tens(kUp), tens(kDown));
        problem_ = text;
        return;
    }
    KnobModel m;
    m.hzPerKnob = (kUp + kDown) / 2;
    const double k = m.hzPerKnob;
    // The edges with the knobs centred, from the turns that moved them.
    double low = 0, high = 0;
    if (k > 0)
    {
        low = ((spans_[1].lowHz - k * up1) + (spans_[2].lowHz - k * up2)) / 2;
        high = ((spans_[3].highHz - k * down1) + (spans_[4].highHz - k * down2)) / 2;
    }
    else
    {
        high = ((spans_[1].highHz - k * up1) + (spans_[2].highHz - k * up2)) / 2;
        low = ((spans_[3].lowHz - k * down1) + (spans_[4].lowHz - k * down2)) / 2;
    }
    m.centreHz = (low + high) / 2;
    m.widthHz = high - low;
    m.nominalHz = original_.widthHz;
    // An edge the centred measure didn't show where it is: a cut in the
    // audio hides it.
    if (centred.lowHz > low + LEAST_HZ) m.floorHz = centred.lowHz;
    if (centred.highHz < high - LEAST_HZ) m.ceilingHz = centred.highHz;
    result_ = m;
}

std::string Calibration::stepText() const
{
    switch (step_)
    {
    case 0: return "Measuring with the knobs centred (1 of 5)";
    case 1: return "Measuring with PBT_IN turned up (2 of 5)";
    case 2: return "Measuring with PBT_IN turned up further (3 of 5)";
    case 3: return "Measuring with PBT_OUT turned down (4 of 5)";
    case 4: return "Measuring with PBT_OUT turned down further (5 of 5)";
    case PUT_BACK: return "Putting the knobs back";
    default: break;
    }
    return std::string();
}

} // namespace ear
} // namespace ragtime
