#include "EarFilterBar.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/button.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include "EarControls.h"
#include "EarScope.h"
#include "Prefs.h"
#include "ragtime/ear/AutoSpan.h"
#include "ragtime/ear/Passbands.h"

using namespace ragtime;
using ear::FilterEdit;
using ear::Span;
using radio::RigFilter;

namespace
{

// A calibration step gives up after this long.
constexpr uint64_t STEP_GIVE_UP_MS = 45000;
// Measured afresh after a write: this long for the rig to take it, and
// this much quiet in the new average.
constexpr uint64_t SETTLE_MS = 1000;
constexpr uint64_t READ_BACK_MS = 3000;
constexpr double CALIBRATE_QUIET_S = 4.0;
// The edges missed what was wanted by more than this.
constexpr double MISS_HZ = 100.0;
// After a write the noise is left out this long: Jeff's IC-7100's AGC
// swells about 10 dB for 5 to 7 s after each knob turn (2026-10-26).
constexpr double AGC_SETTLE_S = 7.0;
// The notch's dip is looked for this far either side of where the model
// puts it, and the passband is bridged over as far.
constexpr double DIP_SEARCH_HZ = 450.0;
// A dip this close to an edge is no measure of the notch's centre.
constexpr double DIP_EDGE_HZ = 170.0;

const wxString DOT = wxString::FromUTF8(" \xc2\xb7 ");
const wxString DASH = wxString::FromUTF8("\xe2\x80\x93");

wxString utf8(const std::string& s)
{
    return wxString::FromUTF8(s);
}

int tens(double hz)
{
    return (int)std::lround(hz / 10.0) * 10;
}

wxString spanText(const Span& s)
{
    return wxString::Format("%d", tens(s.lowHz)) + DASH + wxString::Format("%d Hz", tens(s.highHz));
}

// The rig reads back what was written: the knobs come back in the rig's
// own steps, which can be coarser than Hamlib's 1/255 (Jeff's IC-7100
// reads 0.43 back as 0.44), so within four of those.
bool readsBack(const RigFilter& got, const RigFilter& want)
{
    auto knob = [](const std::optional<double>& a, const std::optional<double>& b) {
        if (!b) return true;
        return a && std::fabs(*a - *b) <= 4.0 / 255.0;
    };
    const bool notch = !want.notchOn || (got.notchOn == want.notchOn && (!*want.notchOn || knob(got.notchRaw, want.notchRaw)));
    return got.mode == want.mode && got.widthHz == want.widthHz && knob(got.pbtIn, want.pbtIn) &&
           knob(got.pbtOut, want.pbtOut) && notch;
}

wxColour warningColour()
{
    return wxColour(210, 40, 40);
}

} // namespace

EarFilterBar::EarFilterBar(wxWindow* parent, EarScope* scope, FilterEdit& edit, Write write)
    : wxPanel(parent)
    , scope_(scope)
    , edit_(edit)
    , write_(std::move(write))
{
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    // Disable, left of the presets (Jeff, 2026-10-26): the PBT centred and
    // the notch off. It shows pushed in while no filter is on, a preset
    // while the rig has what it holds.
    disable_ = new PresetButton(this, "Disable");
    disable_->setDetail("PBT " + wxString::FromUTF8("\xc2\xb7") + " NF");
    disable_->SetToolTip("Centres the PBT and turns the notch off, keeping the rig's width");
    disable_->onTap([this] { disableFilters(); });
    top->Add(disable_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        auto* p = new PresetButton(this, wxString::Format("Preset %d", i + 1));
        p->SetToolTip("A tap sets the rig's filter to this preset; press and hold to store the filter the rig has "
                      "now (kept for this rig and mode)");
        p->onTap([this, i] { recall(i + 1); });
        p->onStore([this, i] { store(i + 1); });
        presets_[(std::size_t)i] = p;
        top->Add(p, 0, wxALIGN_CENTER_VERTICAL | (i > 0 ? wxLEFT : 0), FromDIP(6));
    }
    top->AddStretchSpacer();
    calibrate_ = new wxButton(this, wxID_ANY, "Calibrate");
    calibrate_->SetToolTip("Turns PBT_IN and PBT_OUT a little in turn and measures how far the edges move in the "
                           "band's noise, so the edges can be dragged one at a time. Takes about half a minute of "
                           "quiet band, then puts the knobs back.");
    putBack_ = new wxButton(this, wxID_ANY, "Revert");
    putBack_->SetToolTip("Sets the rig's filter back to what it was when the Ear first read it (quitting Ragtime "
                         "does too)");
    top->Add(calibrate_, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(putBack_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));

    auto* bottom = new wxBoxSizer(wxHORIZONTAL);
    line_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                             wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    // Shortened to fit, the whole of it in the tooltip.
    line_->SetMinSize(FromDIP(wxSize(120, -1)));
    clock_ = new ClockDial(this, 22);
    clock_->SetToolTip("The change goes back when the clock runs out, unless you press Keep. The clock stands "
                       "still while the pointer is on Undo or Keep.");
    undo_ = new wxButton(this, wxID_ANY, "Undo");
    keep_ = new wxButton(this, wxID_ANY, "Keep anyway");
    keep_->SetMinSize(keep_->GetBestSize());
    keep_->SetLabel("Keep");
    bottom->Add(line_, 1, wxALIGN_CENTER_VERTICAL);
    bottom->Add(clock_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    bottom->Add(undo_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    bottom->Add(keep_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    clock_->Hide();
    undo_->Hide();
    keep_->Hide();

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(top, 0, wxEXPAND);
    outer->Add(bottom, 0, wxEXPAND | wxTOP, FromDIP(6));
    SetSizer(outer);

    calibrate_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { calibrate(); });
    putBack_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        from_ = spanNow();
        if (edit_.found()) wanted_ = spanOfFilter(*edit_.found());
        miss_.clear();
        calibrationNews_.clear();
        edit_.putBack();
        if (edges_) show(nowMs_, *edges_);
    });
    undo_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        edit_.undo();
        // Where it goes back to, shown at once.
        wanted_ = edit_.reverting() ? from_ : std::nullopt;
        miss_.clear();
        if (edges_) show(nowMs_, *edges_);
    });
    keep_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // A cut into Data2G's passband is kept only on a second press.
        if (cutsData2G() && !keepArmed_)
        {
            keepArmed_ = true;
            if (edges_) show(nowMs_, *edges_);
            return;
        }
        edit_.keep();
        keepArmed_ = false;
        // Kept ahead of its write, it still shows what is coming.
        if (!edit_.keptAhead()) wanted_.reset();
        if (edges_) show(nowMs_, *edges_);
    });
    scope_->onDrag([this](const Span& from, const Span& to) { dragged(from, to); });
    scope_->onNotch([this](std::optional<double> hz) { notchMoved(hz); });
}

ear::KnobModel EarFilterBar::model(const RigFilter& f) const
{
    if (!rig_.key.empty())
    {
        const std::string key = rig_.key + "/" + f.mode + "/" + std::to_string(f.widthHz);
        if (key != calibrationKey_)
        {
            calibrationKey_ = key;
            calibrated_ = loadEarCalibration(rig_.key, f.mode, f.widthHz);
        }
        if (calibrated_) return *calibrated_;
    }
    const ear::Passband d = ear::data2gPassband(false);
    return ear::KnobModel::about(centre_.value_or((d.lowHz + d.highHz) / 2), f);
}

bool EarFilterBar::onFm() const
{
    return rig_.filter && ear::fmMode(rig_.filter->mode);
}

bool EarFilterBar::calibrated() const
{
    const std::optional<RigFilter> f = setting();
    return f && f->pbtIn && f->pbtOut && model(*f).known();
}

std::optional<RigFilter> EarFilterBar::setting() const
{
    // What Ragtime last wrote, until the rig reads it back; else the rig's.
    if (edit_.current()) return edit_.current();
    return rig_.filter;
}

std::optional<Span> EarFilterBar::spanOfFilter(const RigFilter& f) const
{
    const ear::KnobModel m = model(f);
    return ear::visible(ear::spanOf(f, m), m);
}

std::optional<Span> EarFilterBar::measured(const ear::EdgeFinder& edges) const
{
    const std::optional<ear::Edges> e = edges.edges();
    if (!e || !e->lowHz || !e->highHz) return std::nullopt;
    return Span{*e->lowHz, *e->highHz};
}

std::optional<Span> EarFilterBar::spanNow() const
{
    if (edges_)
        if (auto m = measured(*edges_)) return m;
    // Just written, until the noise shows it: where it was dropped, not
    // where the model puts it, so the edges don't jump to the model and
    // then again to the noise.
    if (landing_) return landing_;
    if (auto f = setting()) return spanOfFilter(*f);
    return std::nullopt;
}

void EarFilterBar::tick(uint64_t nowMs, bool quiet, ear::EdgeFinder& edges, const EarRig& rig, bool narrow)
{
    nowMs_ = nowMs;
    quiet_ = quiet;
    edges_ = &edges;
    narrow_ = narrow;
    rig_ = rig;

    // A new reading: the noise's average starts afresh with it.
    if (rig.filter && (!lastFilter_ || *lastFilter_ != *rig.filter))
    {
        if (lastFilter_) edges.restart();
        lastFilter_ = rig.filter;
        edit_.seen(*rig.filter);
        miss_.clear();
    }

    // Not calibrated: where the passband sits with the knobs centred, from
    // the noise (Calibrate learns it better, past any cut in the audio).
    if (!calibration_.running() && edit_.state() == FilterEdit::State::Idle && rig.filter && edit_.current() &&
        readsBack(*rig.filter, *edit_.current()) && !model(*rig.filter).known())
        if (auto m = measured(edges)) centre_ = m->centre();

    if (calibration_.running())
    {
        runCalibration(nowMs, quiet, edges);
    }
    else if (restore_)
    {
        if (rig.filter && rig.filter->mode != restore_->mode)
        {
            restore_.reset();
        }
        else if (quiet)
        {
            write(*restore_, "put back after Calibrate", edges);
            restore_.reset();
        }
    }
    else if (rig.canSet)
    {
        // A change going on (not one going back), and what going back
        // returns to.
        const std::optional<RigFilter> forward =
            edit_.state() == FilterEdit::State::Waiting && !edit_.reverting() ? edit_.pending() : std::nullopt;
        const std::optional<RigFilter> back = edit_.before();
        if (auto f = edit_.due(nowMs, quiet))
        {
            const bool on = forward && *f == *forward;
            landing_ = on ? wanted_ : back && *f == *back ? from_ : std::nullopt;
            write(*f, edit_.state() == FilterEdit::State::Trial ? "on trial" : on ? "kept" : "put back", edges);
        }
    }
    else if (edit_.state() != FilterEdit::State::Idle)
    {
        // The rig closed or the box was unticked: an unkept change goes
        // back (a Hamlib rig not open gets it once it is).
        if (auto f = edit_.close()) write(*f, "put back: the rig can't be set now", edges);
        wanted_.reset();
    }

    // The miss: once the rig has the change and the noise has settled, if
    // the change moved the edges.
    const std::optional<RigFilter>& before = edit_.before();
    const std::optional<RigFilter>& pending = edit_.pending();
    const bool edgesMoved = pending && (!before || before->widthHz != pending->widthHz ||
                                        before->pbtIn != pending->pbtIn || before->pbtOut != pending->pbtOut);
    if (edit_.state() == FilterEdit::State::Trial && miss_.empty() && wanted_ && edgesMoved && rig.filter &&
        readsBack(*rig.filter, *edit_.pending()) && nowMs >= writtenMs_ + SETTLE_MS &&
        edges.quietSeconds() >= ear::EdgeFinder::NEEDED_S)
    {
        if (auto m = measured(edges))
        {
            // What the noise can show of what was wanted, past a cut.
            const Span want = ear::visible(*wanted_, model(*edit_.pending()));
            const double off = std::max(std::fabs(m->lowHz - want.lowHz), std::fabs(m->highHz - want.highHz));
            miss_ = std::string((off > MISS_HZ ? "missed by " + std::to_string(tens(off)) + " Hz: noise shows "
                                               : std::string("noise shows ")) +
                                std::string(spanText(*m).utf8_str()));
        }
    }

    // The clock stands still while the pointer is on Undo or Keep.
    const wxPoint mouse = wxGetMousePosition();
    edit_.hold(trialShown_ && (undo_->GetScreenRect().Contains(mouse) || keep_->GetScreenRect().Contains(mouse)),
               nowMs);

    showNotch(edges);

    // The noise shows the edges again: they are the truth.
    if (landing_ && measured(edges)) landing_.reset();

    // The handles, while the operator may drag.
    const bool canDrag = settable() && setting() && !calibration_.running() && !restore_ &&
                         edit_.state() != FilterEdit::State::Waiting;
    const std::optional<RigFilter> f = setting();
    scope_->setHandles(canDrag ? spanNow() : std::nullopt, !calibrated(), f && f->pbtIn && f->pbtOut, quiet_);
    if (edit_.state() == FilterEdit::State::Idle) wanted_.reset();
    scope_->setWanted(wanted_);

    if (++ticks_ % 2 == 0 || edit_.state() == FilterEdit::State::Trial) show(nowMs, edges);
}

void EarFilterBar::closing()
{
    if (calibration_.running())
    {
        calibration_.cancel();
        if (calibrationFrom_) write_(*calibrationFrom_, "put back: the Ear closed during Calibrate");
    }
    else if (restore_)
    {
        write_(*restore_, "put back after Calibrate");
    }
    restore_.reset();
    if (auto f = edit_.close()) write_(*f, "put back: the Ear closed");
    wanted_.reset();
}

void EarFilterBar::dragged(const Span& from, const Span& wanted)
{
    const std::optional<RigFilter> now = setting();
    if (!settable() || !now || calibration_.running()) return;
    ear::KnobModel m = model(*now);
    // Uncalibrated, a shift turns both knobs alike by a guessed Hz per
    // knob (the noise shows where it lands), and an edge moves only the
    // width, about the centre the passband has now.
    // Shown where it lands, not where it was dropped, when the knobs stop
    // it short (ear::reached).
    const double dl = wanted.lowHz - from.lowHz, dh = wanted.highHz - from.highHz;
    if (!m.known() && std::fabs(dl - dh) < 1 && now->pbtIn && now->pbtOut)
    {
        m.hzPerKnob = ear::guessHzPerKnob(*now);
        const RigFilter to = ear::shiftedBy(*now, dl, m.hzPerKnob);
        change(to, ear::reached(from, *now, to, m));
        return;
    }
    if (!m.known()) m.centreHz = from.centre();
    const RigFilter to = ear::settingFor(ear::dragTarget(from, wanted, *now, m), *now, m);
    change(to, ear::reached(from, *now, to, m));
}

const ear::NotchModel& EarFilterBar::notchModel()
{
    const std::string key = rig_.filter ? rig_.key + "/" + rig_.filter->mode : std::string();
    if (key != notchKey_)
    {
        notchKey_ = key;
        notchModel_ = rig_.key.empty() || !rig_.filter ? ear::NotchModel() : loadEarNotch(rig_.key, rig_.filter->mode);
        learnedRaw_.reset();
        dipHz_.reset();
    }
    return notchModel_;
}

void EarFilterBar::showNotch(ear::EdgeFinder& edges)
{
    const ear::NotchModel& m = notchModel();
    const std::optional<RigFilter>& rig = rig_.filter;
    // The rig's notch as read: the noise's run is bridged over it, and its
    // dip looked for around where the model puts it.
    const bool on = rig && rig->notchOn && *rig->notchOn && rig->notchRaw;
    if (on)
    {
        const double hz = m.hzOf(*rig->notchRaw);
        edges.setBridge(hz - DIP_SEARCH_HZ, hz + DIP_SEARCH_HZ);
        dipHz_.reset();
        if (auto d = edges.dip(hz - DIP_SEARCH_HZ, hz + DIP_SEARCH_HZ))
        {
            dipHz_ = d->hz;
            // Learned from once for each position, well inside the passband
            // (an edge or a cut in the audio drags a dip's floor sideways),
            // and once the rig has what was written.
            const std::optional<Span> pass = measured(edges);
            const bool inside = pass && d->hz > pass->lowHz + DIP_EDGE_HZ && d->hz < pass->highHz - DIP_EDGE_HZ;
            const bool settled = !edit_.pending() || readsBack(*rig, *edit_.pending());
            if (inside && settled && (!learnedRaw_ || std::fabs(*learnedRaw_ - *rig->notchRaw) > 0.5 / 255.0))
            {
                learnedRaw_ = rig->notchRaw;
                notchModel_.learn(*rig->notchRaw, d->hz);
                if (!rig_.key.empty()) saveEarNotch(rig_.key, rig->mode, notchModel_);
            }
        }
    }
    else
    {
        edges.setBridge(0, 0);
        dipHz_.reset();
    }

    // The bar: where the rig has a notch. It shows the change waiting or on
    // trial, else the setting.
    const std::optional<RigFilter> f = edit_.pending() ? edit_.pending() : setting();
    if (!f || !f->notchOn || onFm())
    {
        scope_->setNotch(std::nullopt);
        return;
    }
    EarScope::Notch n;
    n.on = *f->notchOn && f->notchRaw;
    if (n.on) n.hz = notchModel_.hzOf(*f->notchRaw);
    if (on && n.on && rig->notchRaw == f->notchRaw) n.dipHz = dipHz_;
    n.movable = adjustable() && !calibration_.running() && !restore_ && edit_.state() != FilterEdit::State::Waiting;
    scope_->setNotch(n);
}

void EarFilterBar::notchMoved(std::optional<double> hz)
{
    const std::optional<RigFilter> now = setting();
    if (!settable() || !now || !now->notchOn || calibration_.running()) return;
    // The edges stay as the noise shows them.
    change(ear::withNotch(*now, hz, notchModel()), spanNow());
}

bool EarFilterBar::cutsData2G() const
{
    const std::optional<Span> s = wanted_ ? wanted_ : spanNow();
    if (s && ear::cutsData2G(*s, narrow_)) return true;
    const std::optional<RigFilter>& p = edit_.pending();
    return p && p->notchOn && *p->notchOn && p->notchRaw &&
           ear::notchCutsData2G(notchModel_.hzOf(*p->notchRaw), narrow_);
}

void EarFilterBar::change(const RigFilter& to, const std::optional<Span>& wanted)
{
    if (edit_.state() == FilterEdit::State::Idle)
    {
        if (edit_.current() && *edit_.current() == to)
        {
            line_->SetLabel("No change: the rig has that already");
            return;
        }
        from_ = spanNow();
    }
    wanted_ = wanted ? wanted : spanOfFilter(to);
    keepArmed_ = false;
    miss_.clear();
    calibrationNews_.clear();
    edit_.change(to, nowMs_);
    if (edges_) show(nowMs_, *edges_);
}

void EarFilterBar::recall(int n)
{
    if (!settable() || !rig_.filter || calibration_.running()) return;
    const std::optional<RigFilter> p = loadEarPreset(rig_.key, rig_.filter->mode, n);
    if (!p)
    {
        line_->SetForegroundColour(GetForegroundColour());
        line_->SetLabel(wxString::Format("Preset %d is empty: press and hold it to store the filter the rig has now",
                                         n));
        return;
    }
    // What the preset holds over what the rig has: a preset from before the
    // notch leaves the notch as it is.
    RigFilter to = *rig_.filter;
    to.mode = p->mode;
    to.widthHz = p->widthHz;
    if (p->pbtIn) to.pbtIn = p->pbtIn;
    if (p->pbtOut) to.pbtOut = p->pbtOut;
    if (p->notchOn) to.notchOn = p->notchOn;
    if (p->notchRaw) to.notchRaw = p->notchRaw;
    change(to, std::nullopt);
}

void EarFilterBar::disableFilters()
{
    if (!settable() || !rig_.filter || calibration_.running()) return;
    change(ear::disabled(*rig_.filter), std::nullopt);
}

void EarFilterBar::store(int n)
{
    if (!rig_.filter || rig_.key.empty()) return;
    saveEarPreset(rig_.key, n, *rig_.filter);
    presetsKey_.clear();
    showPresets();
}

void EarFilterBar::calibrate()
{
    if (calibration_.running())
    {
        calibration_.cancel();
        restore_ = calibrationFrom_;
        calibrationNews_ = "Calibrate stopped";
    }
    else if (settable() && rig_.filter && edit_.state() == FilterEdit::State::Idle &&
             calibration_.start(*rig_.filter))
    {
        calibrationFrom_ = rig_.filter;
        calibrationWritten_ = false;
        calibrationStepMs_ = nowMs_;
        calibrationNews_.clear();
        wanted_.reset();
    }
    if (edges_) show(nowMs_, *edges_);
}

void EarFilterBar::runCalibration(uint64_t nowMs, bool quiet, ear::EdgeFinder& edges)
{
    const std::optional<RigFilter> want = calibration_.setting();
    if (!want) return;
    if (rig_.filter && calibrationFrom_ && rig_.filter->mode != calibrationFrom_->mode)
    {
        // The operator turned the mode knob: writing on would turn it back.
        calibration_.cancel();
        calibrationNews_ = "Calibrate stopped: the rig's mode changed";
        return;
    }
    if (!rig_.canSet || nowMs >= calibrationStepMs_ + STEP_GIVE_UP_MS)
    {
        calibration_.cancel();
        restore_ = calibrationFrom_;
        calibrationNews_ = rig_.canSet ? "Calibrate gave up: no clear edges in the noise for 45 s (a busy or very "
                                         "quiet band?)"
                                       : "Calibrate stopped: the rig can't be set now";
        return;
    }
    if (!calibrationWritten_)
    {
        if (!quiet) return;
        write(*want, calibration_.stepText(), edges);
        calibrationWritten_ = true;
        return;
    }
    const bool settled = (rig_.filter && readsBack(*rig_.filter, *want)) || nowMs >= writtenMs_ + READ_BACK_MS;
    if (!settled || nowMs < writtenMs_ + SETTLE_MS) return;
    if (calibration_.step() == ear::Calibration::PUT_BACK)
    {
        calibration_.measured(Span{});
        finishCalibration();
        return;
    }
    const std::optional<Span> m = measured(edges);
    if (!m || edges.quietSeconds() < CALIBRATE_QUIET_S) return;
    calibration_.measured(*m);
    calibrationWritten_ = false;
    calibrationStepMs_ = nowMs;
    // After the last measure the knobs go back; the result waits for that.
}

void EarFilterBar::finishCalibration()
{
    if (calibration_.result() && calibrationFrom_)
    {
        const ear::KnobModel& m = *calibration_.result();
        saveEarCalibration(rig_.key, calibrationFrom_->mode, calibrationFrom_->widthHz, m);
        calibrationKey_.clear();
        // Short enough to fit the line (Hamlet, 2026-10-26: the longer
        // wording was cut off); the tooltip has it too.
        std::string news = "Calibrated: " + std::to_string(std::abs(tens(m.hzPerKnob))) + " Hz a turn, " +
                           std::to_string(tens(m.widthHz)) + " Hz wide about " + std::to_string(tens(m.centreHz)) +
                           " Hz";
        if (m.floorHz > 0) news += ", cut below " + std::to_string(tens(m.floorHz)) + " Hz";
        if (m.ceilingHz > 0) news += ", cut above " + std::to_string(tens(m.ceilingHz)) + " Hz";
        calibrationNews_ = news + ". Drag an edge or the middle.";
    }
    else
    {
        calibrationNews_ = calibration_.problem();
    }
    calibration_.cancel();
    presetsKey_.clear();
}

void EarFilterBar::write(const RigFilter& f, const std::string& why, ear::EdgeFinder& edges)
{
    write_(f, why);
    edges.restart(AGC_SETTLE_S);
    writtenMs_ = nowMs_;
}

void EarFilterBar::showPresets()
{
    const bool on = adjustable() && rig_.filter && !calibration_.running();
    for (PresetButton* p : presets_) p->Enable(on);
    disable_->Enable(on);
    // Pushed in: what the rig has now, or was last given.
    const std::optional<RigFilter> f = setting();
    disable_->setPushed(f && ear::filtersOff(*f));
    for (std::size_t i = 0; i < presets_.size(); i++)
        presets_[i]->setPushed(f && presetFilters_[i] && ear::holds(*f, *presetFilters_[i]));
    const std::string key = rig_.filter ? rig_.key + "/" + rig_.filter->mode + "/" + calibrationKey_ : std::string();
    if (key == presetsKey_) return;
    presetsKey_ = key;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        wxString detail = wxString::FromUTF8("\xe2\x80\x94");
        presetFilters_[(std::size_t)i].reset();
        if (rig_.filter && !rig_.key.empty())
            if (auto p = loadEarPreset(rig_.key, rig_.filter->mode, i + 1))
            {
                presetFilters_[(std::size_t)i] = p;
                const std::optional<ear::KnobModel> m = loadEarCalibration(rig_.key, p->mode, p->widthHz);
                if (m && p->pbtIn && p->pbtOut)
                    detail = spanText(ear::visible(ear::spanOf(*p, *m), *m));
                else
                    detail = wxString::Format("%d Hz", p->widthHz);
                if (p->notchOn && *p->notchOn) detail += " + NF";
            }
        presets_[(std::size_t)i]->setDetail(detail);
    }
}

void EarFilterBar::show(uint64_t nowMs, const ear::EdgeFinder&)
{
    showPresets();
    const FilterEdit::State state = edit_.state();
    const bool idle = state == FilterEdit::State::Idle;
    const std::optional<RigFilter> f = setting();
    const bool calibrating = calibration_.running();

    calibrate_->SetLabel(calibrating ? "Stop" : "Calibrate");
    calibrate_->Enable(calibrating ||
                       (adjustable() && f && f->pbtIn && f->pbtOut && idle && !restore_ && !edit_.reverting()));
    putBack_->Enable(adjustable() && idle && !calibrating && !restore_ && edit_.found() && edit_.current() &&
                     *edit_.found() != *edit_.current());

    wxString text;
    bool warn = false;
    const bool trial = state == FilterEdit::State::Trial;
    const ear::Passband d = ear::data2gPassband(narrow_);
    if (calibrating)
    {
        text = utf8(calibration_.stepText());
        if (!calibrationWritten_) text += DOT + "waiting for a quiet moment";
    }
    else if (restore_)
    {
        text = "Putting the knobs back" + DOT + "waiting for a quiet moment";
    }
    else if (edit_.reverting())
    {
        text = "Putting it back" + DOT + "waiting for the burst to end";
    }
    else if (state == FilterEdit::State::Waiting || trial)
    {
        // The edges' change and the notch's.
        const std::string edges = from_ && wanted_ ? ear::changeText(*from_, *wanted_) : std::string();
        const std::string notch = edit_.before() && edit_.pending()
                                      ? ear::notchChangeText(*edit_.before(), *edit_.pending(), notchModel_)
                                      : std::string();
        if (!notch.empty())
            text = utf8(edges.empty() || edges == "No change" ? notch : edges + ", " + notch);
        else if (!edges.empty())
            text = utf8(edges);
        else
            text = "Changing the filter";
        if (edit_.keptAhead())
            text += DOT + "kept: written when the burst ends";
        else if (state == FilterEdit::State::Waiting)
            text += DOT + "waiting for the burst to end";
        else if (!miss_.empty())
            text += DOT + utf8(miss_);
        if (cutsData2G())
        {
            // First, so the line never cuts it off (Jeff, 2026-10-26: the
            // end of a long line, "Keep twice", went unseen).
            warn = true;
            const wxString cut = "Cuts into Data2G's " + spanText(Span{d.lowHz, d.highHz});
            text = edit_.keptAhead() ? cut + DOT + text
                                     : cut + (keepArmed_ ? ": press Keep again" : ": press Keep twice") + DOT + text;
        }
    }
    else if (!rig_.problem.empty())
    {
        warn = true;
        text = "The rig refused: " + utf8(rig_.problem);
    }
    else if (!calibrationNews_.empty())
    {
        text = utf8(calibrationNews_);
    }
    else if (rig_.via.empty())
    {
        text = "No rig in Settings > Rig: the Ear only shows the filter";
    }
    else if (onFm())
    {
        text = "On FM the rig's filter is the IF's, and PBT does nothing: the Ear only shows the audio";
    }
    else if (!rig_.allowed)
    {
        text = "To drag the edges, tick \"Let the Ear change the rig's filters\" in Settings > Rig";
    }
    else if (!rig_.canSet)
    {
        text = rig_.via == "rigctld" ? "rigctld isn't answering, so the filter can't be set"
                                     : "The rig isn't open, so its filter can't be set";
    }
    else if (!f)
    {
        text = "Waiting for the rig to say its filter";
    }
    else if (!quiet_)
    {
        text = "On the air: the filter holds still until the burst ends";
    }
    else if (!calibrated())
    {
        text = f->pbtIn && f->pbtOut ? "Drag the middle to shift, or an edge for the width. Calibrate frees each "
                                       "edge"
                                     : "Drag an edge to change the width: both edges move about the centre";
    }
    else
    {
        text = f->notchOn ? "Drag an edge, the middle, or NF from its bay. A change goes back in 15 s unless kept"
                          : "Drag an edge or the middle. A change goes back in 15 s unless kept";
    }

    const wxColour colour = warn ? warningColour() : GetForegroundColour();
    if (line_->GetForegroundColour() != colour) line_->SetForegroundColour(colour);
    if (line_->GetLabel() != text)
    {
        line_->SetLabel(text);
        line_->SetToolTip(text);
    }

    // The clock, Undo and Keep: while a change is waiting or on trial.
    const bool showTrial =
        !calibrating && !restore_ && !edit_.reverting() && (trial || state == FilterEdit::State::Waiting);
    if (showTrial != trialShown_)
    {
        trialShown_ = showTrial;
        clock_->Show(showTrial);
        undo_->Show(showTrial);
        keep_->Show(showTrial);
        Layout();
    }
    if (showTrial)
    {
        clock_->setLeft(trial ? edit_.secondsLeft(nowMs) / (FilterEdit::KEEP_MS / 1000.0) : 1.0);
        clock_->setColour(warn ? warningColour() : wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT));
        // While waiting too: kept as it is written (Jeff, 2026-10-26).
        keep_->Enable(trial || (state == FilterEdit::State::Waiting && !edit_.keptAhead()));
        keep_->SetLabel(keepArmed_ ? "Keep anyway" : "Keep");
        // Going back writes: not while on the air. Keep writes nothing.
        undo_->Enable(quiet_);
        undo_->SetToolTip(trial ? "Put the filter back as it was before the change"
                                : "Forget the change before it is written");
    }
}
