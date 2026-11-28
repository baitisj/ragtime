#include "ragtime/ear/FilterControl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

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

bool NotchModel::learnWidth(std::optional<int> step, double hz)
{
    const int i = step ? std::max(0, std::min(*step, 15)) : 0;
    if (hz < 20 || hz > 4000) return false;
    if ((int)widthsHz.size() <= i) widthsHz.resize(i + 1, 0.0);
    double& w = widthsHz[i];
    // A little at a time once known: one dip in a busy band can be off.
    const double next = w > 0 ? w + (hz - w) * 0.3 : hz;
    const bool moved = std::fabs(next - w) >= 5;
    w = next;
    return moved;
}

double NotchModel::widthAt(std::optional<int> step, int steps) const
{
    const int n = std::max(1, steps);
    const int i = steps > 0 && step ? std::max(0, std::min(*step, n - 1)) : 0;
    if (i < (int)widthsHz.size() && widthsHz[i] > 0) return widthsHz[i];
    // The nearest step measured, each step 1.5 times the one before.
    int nearest = -1;
    for (int k = 0; k < std::min(n, (int)widthsHz.size()); k++)
        if (widthsHz[k] > 0 && (nearest < 0 || std::abs(k - i) < std::abs(nearest - i))) nearest = k;
    if (nearest >= 0) return widthsHz[nearest] * std::pow(1.5, i - nearest);
    return n == 1 ? WIDTH_HZ : 200.0 + 200.0 * i / (n - 1);
}

int NotchModel::stepFor(double hz, int steps) const
{
    int best = 0;
    for (int k = 1; k < std::max(1, steps); k++)
        if (std::fabs(widthAt(k, steps) - hz) < std::fabs(widthAt(best, steps) - hz)) best = k;
    return best;
}

std::string NotchModel::stepName(std::optional<int> step, int steps) const
{
    if (!step || steps < 2) return std::string();
    // Its rank by width among the steps.
    const double w = widthAt(step, steps);
    int rank = 0;
    for (int k = 0; k < steps; k++)
        if (k != *step && (widthAt(k, steps) < w || (widthAt(k, steps) == w && k < *step))) rank++;
    if (steps == 2) return rank == 0 ? "Narrow" : "Wide";
    if (steps == 3) return rank == 0 ? "Narrow" : rank == 1 ? "Mid" : "Wide";
    return "width " + std::to_string(rank + 1);
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
    for (double w : widthsHz)
    {
        if (w > 0)
            out << ' ' << w;
        else
            out << " -";
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
    // The widths, if any (an older one has none).
    std::string w;
    while (in >> w && m.widthsHz.size() < 16)
    {
        std::istringstream v(w);
        v.imbue(std::locale::classic());
        double hz = 0;
        m.widthsHz.push_back(w != "-" && (v >> hz) && hz > 0 ? hz : 0.0);
    }
    return m;
}

std::optional<double> notchCentre(const RigFilter& f, const NotchModel& m)
{
    if (f.notchHz) return (double)*f.notchHz;
    if (f.notchRaw) return m.hzOf(*f.notchRaw);
    return std::nullopt;
}

bool notchCutsData2G(double hz, bool narrow, double widthHz)
{
    const Passband p = data2gPassband(narrow);
    return hz + widthHz / 2 > p.lowHz && hz - widthHz / 2 < p.highHz;
}

double notchWidthOf(const RigFilter& f, const NotchModel& m)
{
    return m.widthAt(f.notchWidth, f.notchWidths);
}

RigFilter withNotchWidth(const RigFilter& now, int step)
{
    RigFilter out = now;
    if (now.notchWidths > 0) out.notchWidth = std::max(0, std::min(step, now.notchWidths - 1));
    return out;
}

std::string notchChangeText(const RigFilter& from, const RigFilter& to, const NotchModel& m)
{
    const bool was = from.notchOn && *from.notchOn, is = to.notchOn && *to.notchOn;
    if (!was && !is) return std::string();
    if (!is) return "Notch off";
    const std::optional<double> toHz = notchCentre(to, m), fromHz = notchCentre(from, m);
    if (!was) return toHz ? "Notch on at " + std::to_string(tens(*toHz)) + " Hz" : std::string("Notch on");
    // Its width, where that alone changed.
    const bool widens = to.notchWidth && from.notchWidth && *to.notchWidth != *from.notchWidth;
    const std::string width = widens ? "Notch " + m.stepName(from.notchWidth, from.notchWidths) + ARROW +
                                           m.stepName(to.notchWidth, to.notchWidths)
                                     : std::string();
    if (!toHz || !fromHz) return width;
    const int at = tens(*toHz), before = tens(*fromHz);
    if (before == at) return width;
    return "Notch " + std::to_string(before) + ARROW + std::to_string(at) + " Hz" +
           (widens ? ", " + m.stepName(to.notchWidth, to.notchWidths) : std::string());
}

RigFilter withNotch(const RigFilter& now, std::optional<double> hz, const NotchModel& m)
{
    RigFilter out = now;
    out.notchOn = hz.has_value();
    if (hz && now.notchHz && !now.notchRaw)
        out.notchHz = std::max(10, tens(*hz));
    else if (hz)
        out.notchRaw = m.rawFor(*hz);
    return out;
}

bool twinPbt(const RigFilter& f)
{
    return f.pbtIn && f.pbtOut;
}

bool ifShifts(const RigFilter& f)
{
    return !twinPbt(f) && f.ifShiftHz.has_value();
}

int calibrationWidth(const RigFilter& f)
{
    return ifShifts(f) ? 0 : f.widthHz;
}

std::vector<int> widthSteps(const std::string& rigKey, const std::string& mode)
{
    // An FT-891 (Hamlib's model 1036): the widths it takes, which Hamlib
    // rounds a width up to.
    if (rigKey == "hamlib-1036")
    {
        if (mode == "USB" || mode == "LSB")
            return {200,  400,  600,  850,  1100, 1350, 1500, 1650, 1800, 1950, 2100,
                    2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000, 3200};
        if (mode == "PKTUSB" || mode == "PKTLSB" || mode == "CW" || mode == "CWR" || mode == "RTTY" ||
            mode == "RTTYR")
            return {50,  100, 150, 200, 250, 300,  350,  400,  450,
                    500, 800, 1200, 1400, 1700, 2000, 2400, 3000};
    }
    return {};
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
    // IF SHIFT moves both edges alike, as both knobs turned together.
    const double shift = ifShifts(f) ? m.hzPerKnob * *f.ifShiftHz : 0.0;
    const double in = knobShift(f.pbtIn, m.hzPerKnob) + shift, out = knobShift(f.pbtOut, m.hzPerKnob) + shift;
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

Span reached(const Span& from, const RigFilter& now, const RigFilter& to, const KnobModel& m)
{
    const Span was = visible(spanOf(now, m), m), got = visible(spanOf(to, m), m);
    return visible(Span{got.lowHz + (from.lowHz - was.lowHz), got.highHz + (from.highHz - was.highHz)}, m);
}

double centreOf(const Span& measured, const RigFilter& f, double hzPerKnob)
{
    const double shift = ifShifts(f) ? hzPerKnob * *f.ifShiftHz : 0.0;
    return measured.centre() - (knobShift(f.pbtIn, hzPerKnob) + knobShift(f.pbtOut, hzPerKnob)) / 2 - shift;
}

int roundWidth(double hz, bool up, const std::vector<int>& steps)
{
    if (!steps.empty())
    {
        if (up)
        {
            for (int w : steps)
                if (w >= hz - 1e-9) return w;
            return steps.back();
        }
        int best = steps.front();
        for (int w : steps)
            if (std::fabs(w - hz) < std::fabs(best - hz)) best = w;
        return best;
    }
    hz = std::max(50.0, hz);
    const double step = hz <= 500 ? 50.0 : 100.0;
    const double n = up ? std::ceil(hz / step - 1e-9) : std::round(hz / step);
    return std::max(50, (int)(n * step));
}

RigFilter settingFor(const Span& wanted, const RigFilter& now, const KnobModel& m, const std::vector<int>& steps)
{
    RigFilter out = now;
    const double want = std::max(50.0, wanted.width());
    // The rig's widths are nominal: what it passes is this much more.
    const double extra = m.widthHz - m.nominalHz;
    if (m.known() && ifShifts(now))
    {
        // IF SHIFT: the nearest width the rig takes, centred by the shift,
        // in its steps and within its reach.
        out.widthHz = roundWidth(want - extra, false, steps);
        const double shift = (wanted.centre() - m.centreHz) / m.hzPerKnob;
        const int steps20 = (int)std::lround(shift / IF_SHIFT_STEP_HZ);
        out.ifShiftHz = std::max(-IF_SHIFT_MAX_HZ, std::min(IF_SHIFT_MAX_HZ, steps20 * IF_SHIFT_STEP_HZ));
        return out;
    }
    const bool knobs = m.known() && now.pbtIn && now.pbtOut;
    if (!knobs)
    {
        // The width alone: the edges move together about the centre.
        out.widthHz = roundWidth(want - extra, false, steps);
        return out;
    }
    int width = now.widthHz > 0 ? now.widthHz : roundWidth(want - extra, true, steps);
    if (want > width + extra) width = roundWidth(want - extra, true, steps);
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
        width = roundWidth(want - extra, true, steps);
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
    if (ifShifts(now) && hzPerKnob != 0)
    {
        const int to = *now.ifShiftHz + (int)std::lround(hz / hzPerKnob / IF_SHIFT_STEP_HZ) * IF_SHIFT_STEP_HZ;
        out.ifShiftHz = std::max(-IF_SHIFT_MAX_HZ, std::min(IF_SHIFT_MAX_HZ, to));
        return out;
    }
    if (!now.pbtIn || !now.pbtOut || hzPerKnob <= 0) return out;
    const double in = *now.pbtIn, o = *now.pbtOut;
    const double turn = std::max(-std::min(in, o), std::min(1 - std::max(in, o), hz / hzPerKnob));
    out.pbtIn = quantise(in + turn);
    out.pbtOut = quantise(o + turn);
    return out;
}

double guessHzPerKnob(const RigFilter& f)
{
    // IF SHIFT is in Hz: taken as set.
    if (ifShifts(f)) return 1.0;
    return f.widthHz > 0 ? f.widthHz : 3000;
}

RigFilter disabled(const RigFilter& now)
{
    RigFilter out = now;
    if (out.pbtIn) out.pbtIn = quantise(0.5);
    if (out.pbtOut) out.pbtOut = quantise(0.5);
    if (ifShifts(out)) out.ifShiftHz = 0;
    if (out.notchOn) out.notchOn = false;
    return out;
}

bool filtersOff(const RigFilter& f)
{
    return (!f.pbtIn || radio::pbtCentred(*f.pbtIn)) && (!f.pbtOut || radio::pbtCentred(*f.pbtOut)) &&
           !(ifShifts(f) && *f.ifShiftHz != 0) && !(f.notchOn && *f.notchOn);
}

bool holds(const RigFilter& rig, const RigFilter& preset)
{
    auto near = [](const std::optional<double>& a, const std::optional<double>& b) {
        return !b || (a && std::fabs(*a - *b) <= 1.5 / 255.0);
    };
    if (rig.mode != preset.mode || rig.widthHz != preset.widthHz) return false;
    if (!near(rig.pbtIn, preset.pbtIn) || !near(rig.pbtOut, preset.pbtOut)) return false;
    if (preset.ifShiftHz && !(rig.ifShiftHz && std::abs(*rig.ifShiftHz - *preset.ifShiftHz) <= IF_SHIFT_STEP_HZ))
        return false;
    if (!preset.notchOn) return true;
    const bool on = rig.notchOn && *rig.notchOn;
    if (on != *preset.notchOn) return false;
    if (!on) return true;
    if (preset.notchWidth && rig.notchWidth != preset.notchWidth) return false;
    if (preset.notchHz) return rig.notchHz && std::abs(*rig.notchHz - *preset.notchHz) <= 15;
    return near(rig.notchRaw, preset.notchRaw);
}

bool cutsData2G(const Span& span, bool narrow)
{
    const Passband p = data2gKept(narrow);
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
    keepAhead_ = false;
    state_ = State::Waiting;
}

void FilterEdit::keep()
{
    if (state_ == State::Waiting && !revert_)
    {
        keepAhead_ = true;
        return;
    }
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
        keepAhead_ = false;
        pending_.reset();
        before_.reset();
        return;
    }
    pending_ = before_;
    revert_ = true;
    keepAhead_ = false;
    state_ = State::Waiting;
}

void FilterEdit::putBack()
{
    if (!found_) return;
    pending_ = found_;
    revert_ = true;
    keepAhead_ = false;
    state_ = State::Waiting;
}

std::optional<RigFilter> FilterEdit::due(uint64_t nowMs, bool quiet)
{
    if (state_ == State::Trial && !held_ && nowMs >= deadlineMs_) undo();
    if (state_ != State::Waiting || !quiet || !pending_) return std::nullopt;
    RigFilter write = *pending_;
    everWritten_ = true;
    if (revert_ || keepAhead_)
    {
        // Put back, or kept already: no trial.
        state_ = State::Idle;
        revert_ = false;
        keepAhead_ = false;
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
    revert_ = dirty_ = keepAhead_ = false;
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
    if (!shifts(now)) return false;
    ifShift_ = ifShifts(now);
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
    if (ifShift_)
    {
        static const int SHIFTS[MEASURES] = {0, IF_STEP_HZ, 2 * IF_STEP_HZ, -IF_STEP_HZ, -2 * IF_STEP_HZ};
        f.ifShiftHz = SHIFTS[step_];
        return f;
    }
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
    // The knobs' turns, as the rig takes them; or the shifts.
    const double up1 = ifShift_ ? IF_STEP_HZ : quantise(0.5 + STEP) - 0.5;
    const double up2 = ifShift_ ? 2 * IF_STEP_HZ : quantise(0.5 + 2 * STEP) - 0.5;
    const double down1 = ifShift_ ? -IF_STEP_HZ : quantise(0.5 - STEP) - 0.5;
    const double down2 = ifShift_ ? -2 * IF_STEP_HZ : quantise(0.5 - 2 * STEP) - 0.5;
    // Each pair of turns moved one edge; between its two turns, both in
    // sight. A shift moves both, but one may sit under a cut in the audio:
    // the one that moved further is the measure.
    const bool ifShift = ifShift_;
    auto moved = [ifShift](const Span& a, const Span& b) {
        const double low = b.lowHz - a.lowHz, high = b.highHz - a.highHz;
        if (!ifShift) return low + high;
        return std::fabs(low) >= std::fabs(high) ? low : high;
    };
    const double movedUp = moved(spans_[1], spans_[2]);
    const double movedDown = moved(spans_[3], spans_[4]);
    const double kUp = movedUp / (up2 - up1), kDown = movedDown / (down2 - down1);
    char text[240];
    if (std::fabs(movedUp) < LEAST_HZ || std::fabs(movedDown) < LEAST_HZ)
    {
        if (ifShift_)
            std::snprintf(text, sizeof text,
                          "The edges hardly moved (%d Hz for 200 Hz of shift up, %d Hz down): the rig may not "
                          "take IF SHIFT in this mode, or the band is too quiet to see",
                          tens(movedUp), tens(-movedDown));
        else
            std::snprintf(text, sizeof text,
                          "The edges hardly moved (%d Hz for a turn up, %d Hz for a turn down): this rig's knobs may "
                          "not be the twin PBT, or the band is too quiet to see",
                          tens(movedUp), tens(movedDown));
        problem_ = text;
        return;
    }
    if ((kUp > 0) != (kDown > 0) || std::fabs(kUp - kDown) > AGREE * std::max(std::fabs(kUp), std::fabs(kDown)))
    {
        if (ifShift_)
            std::snprintf(text, sizeof text, "The shifts up and down disagreed (%d and %d Hz for 200 Hz): try again "
                                             "on a steadier band",
                          tens(kUp * IF_STEP_HZ), tens(kDown * IF_STEP_HZ));
        else
            std::snprintf(text, sizeof text, "The two knobs disagreed (%d and %d Hz for a full turn): try again on a "
                                             "steadier band",
                          tens(kUp), tens(kDown));
        problem_ = text;
        return;
    }
    if (ifShift_ && (std::fabs(kUp + kDown) / 2 < IF_LEAST || std::fabs(kUp + kDown) / 2 > IF_MOST))
    {
        std::snprintf(text, sizeof text, "200 Hz of IF SHIFT moved the edges %d Hz: that is no IF SHIFT to go by",
                      tens((kUp + kDown) / 2 * IF_STEP_HZ));
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
    if (ifShift_)
    {
        switch (step_)
        {
        case 0: return "Measuring with IF SHIFT centred (1 of 5)";
        case 1: return "Measuring with IF SHIFT at +200 Hz (2 of 5)";
        case 2: return "Measuring with IF SHIFT at +400 Hz (3 of 5)";
        case 3: return "Measuring with IF SHIFT at -200 Hz (4 of 5)";
        case 4: return "Measuring with IF SHIFT at -400 Hz (5 of 5)";
        case PUT_BACK: return "Putting the shift back";
        default: return std::string();
        }
    }
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
