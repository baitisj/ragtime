#include "EarFilterBar.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/button.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "EarControls.h"
#include "EarScope.h"
#include "Prefs.h"
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

// The rig reads back what was written: the knobs come back as the rig's
// own steps, so within one and a half of them.
bool readsBack(const RigFilter& got, const RigFilter& want)
{
    auto knob = [](const std::optional<double>& a, const std::optional<double>& b) {
        if (!b) return true;
        return a && std::fabs(*a - *b) <= 1.5 / 255.0;
    };
    return got.mode == want.mode && got.widthHz == want.widthHz && knob(got.pbtIn, want.pbtIn) &&
           knob(got.pbtOut, want.pbtOut);
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
    putBack_ = new wxButton(this, wxID_ANY, "Put back as I found them");
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
    clock_->SetToolTip("The change goes back when the clock runs out, unless you press Keep");
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
        wanted_.reset();
        miss_.clear();
        if (edges_) show(nowMs_, *edges_);
    });
    keep_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::optional<Span> s = wanted_ ? wanted_ : spanNow();
        // A cut into Data2G's passband is kept only on a second press.
        if (s && ear::cutsData2G(*s, narrow_) && !keepArmed_)
        {
            keepArmed_ = true;
            if (edges_) show(nowMs_, *edges_);
            return;
        }
        edit_.keep();
        keepArmed_ = false;
        wanted_.reset();
        if (edges_) show(nowMs_, *edges_);
    });
    scope_->onDrag([this](const Span& from, const Span& to) { dragged(from, to); });
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
    if (auto f = setting()) return spanOfFilter(*f);
    return std::nullopt;
}

void EarFilterBar::tick(uint64_t nowMs, bool quiet, ear::EdgeFinder& edges, const EarRig& rig, bool narrow)
{
    nowMs_ = nowMs;
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
        if (auto f = edit_.due(nowMs, quiet))
            write(*f, edit_.state() == FilterEdit::State::Trial ? "on trial" : "put back", edges);
    }
    else if (edit_.state() != FilterEdit::State::Idle)
    {
        // The rig closed or the box was unticked: an unkept change goes
        // back (a Hamlib rig not open gets it once it is).
        if (auto f = edit_.close()) write(*f, "put back: the rig can't be set now", edges);
        wanted_.reset();
    }

    // The miss: once the rig has the change and the noise has settled.
    if (edit_.state() == FilterEdit::State::Trial && miss_.empty() && wanted_ && rig.filter && edit_.pending() &&
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

    // The handles, while the operator may drag.
    const bool canDrag = rig.canSet && setting() && !calibration_.running() && !restore_ &&
                         edit_.state() != FilterEdit::State::Waiting;
    const std::optional<RigFilter> f = setting();
    scope_->setHandles(canDrag ? spanNow() : std::nullopt, !calibrated());
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
    if (!rig_.canSet || !now || calibration_.running()) return;
    ear::KnobModel m = model(*now);
    // Uncalibrated, only the width: about the centre the passband has now.
    if (!m.known()) m.centreHz = from.centre();
    change(ear::settingFor(ear::dragTarget(from, wanted, *now, m), *now, m), wanted);
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
    if (!rig_.canSet || !rig_.filter || calibration_.running()) return;
    const std::optional<RigFilter> p = loadEarPreset(rig_.key, rig_.filter->mode, n);
    if (!p)
    {
        line_->SetForegroundColour(GetForegroundColour());
        line_->SetLabel(wxString::Format("Preset %d is empty: press and hold it to store the filter the rig has now",
                                         n));
        return;
    }
    change(*p, std::nullopt);
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
    else if (rig_.canSet && rig_.filter && edit_.state() == FilterEdit::State::Idle &&
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
        std::string news = "Calibrated: a knob's full turn moves an edge " + std::to_string(std::abs(tens(m.hzPerKnob))) +
                           " Hz, and the filter passes " + std::to_string(tens(m.widthHz)) + " Hz about " +
                           std::to_string(tens(m.centreHz)) + " Hz";
        if (m.floorHz > 0) news += "; the audio cuts below " + std::to_string(tens(m.floorHz)) + " Hz";
        if (m.ceilingHz > 0) news += "; the audio cuts above " + std::to_string(tens(m.ceilingHz)) + " Hz";
        calibrationNews_ = news + ". Drag one edge, or the middle to shift the passband.";
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
    edges.restart();
    writtenMs_ = nowMs_;
}

void EarFilterBar::showPresets()
{
    const bool on = rig_.canSet && rig_.filter && !calibration_.running();
    for (PresetButton* p : presets_) p->Enable(on);
    const std::string key = rig_.filter ? rig_.key + "/" + rig_.filter->mode + "/" + calibrationKey_ : std::string();
    if (key == presetsKey_) return;
    presetsKey_ = key;
    for (int i = 0; i < (int)presets_.size(); i++)
    {
        wxString detail = wxString::FromUTF8("\xe2\x80\x94");
        if (rig_.filter && !rig_.key.empty())
            if (auto p = loadEarPreset(rig_.key, rig_.filter->mode, i + 1))
            {
                const std::optional<ear::KnobModel> m = loadEarCalibration(rig_.key, p->mode, p->widthHz);
                if (m && p->pbtIn && p->pbtOut)
                    detail = spanText(ear::visible(ear::spanOf(*p, *m), *m));
                else
                    detail = wxString::Format("%d Hz", p->widthHz);
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
                       (rig_.canSet && f && f->pbtIn && f->pbtOut && idle && !restore_ && !edit_.reverting()));
    putBack_->Enable(rig_.canSet && idle && !calibrating && !restore_ && edit_.found() && edit_.current() &&
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
        text = "Putting it back" + DOT + "waiting for a quiet moment";
    }
    else if (state == FilterEdit::State::Waiting || trial)
    {
        if (from_ && wanted_)
            text = utf8(ear::changeText(*from_, *wanted_));
        else
            text = "Changing the filter";
        if (state == FilterEdit::State::Waiting)
            text += DOT + "waiting for a quiet moment";
        else if (!miss_.empty())
            text += DOT + utf8(miss_);
        const std::optional<Span> s = wanted_ ? wanted_ : spanNow();
        if (s && ear::cutsData2G(*s, narrow_))
        {
            warn = true;
            text += DOT + "cuts into Data2G's " + spanText(Span{d.lowHz, d.highHz}) +
                    (keepArmed_ ? ": Keep again to keep it" : ": Keep twice to keep it");
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
    else if (!calibrated())
    {
        text = f->pbtIn && f->pbtOut ? "Drag an edge to change the width: both edges move about the centre until "
                                       "Calibrate has measured the PBT knobs"
                                     : "Drag an edge to change the width: both edges move about the centre";
    }
    else
    {
        text = "Drag an edge, or the middle to shift the passband. A change goes back in 15 s unless you press "
               "Keep.";
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
        keep_->Enable(trial);
        keep_->SetLabel(keepArmed_ ? "Keep anyway" : "Keep");
        undo_->SetToolTip(trial ? "Put the filter back as it was before the change"
                                : "Forget the change before it is written");
    }
}
