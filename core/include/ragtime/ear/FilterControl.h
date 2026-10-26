// FilterControl: the Ear setting the rig's receive filter (docs/STAGE1.md,
// "The Ear", step 3). Only with "Let the Ear change the rig's filters"
// ticked (Settings > Rig, off by default), with the rig opened with Hamlib
// or through rigctld (Jeff's OK, 2026-10-26: rigctld is shared, so the box
// is the operator's say-so).
//
// The model. Twin passband tuning runs two filters of the rig's width in a
// row, each shifted by its own knob, and only their overlap passes. With
// the knobs centred the passband is centreHz +- width / 2; a knob at k
// shifts its filter by hzPerKnob * (k - 0.5). So
//
//     low  = centreHz - width / 2 + max(shiftIn, shiftOut)
//     high = centreHz + width / 2 + min(shiftIn, shiftOut)
//
// which is why dragging the low edge up moves one knob, the high edge down
// the other, and the middle both. Nobody publishes hzPerKnob (an Icom's
// PBT reads as the knob's position, 0 to 1), so Calibrate measures it from
// the noise (ear::Calibration), with the passband's real centre and width
// (the rig's width is nominal: Jeff's IC-7100 passes about 3140 Hz at
// -6 dB for its 3000 Hz filter, and a full turn of either knob moves an
// edge 3000 Hz) and any cut in the audio that hides an edge (about 70 Hz
// there): ear::KnobModel. Until then only the width can be set, and
// the edges move together about the centre. Widening past the width sets
// a wider width, as does narrowing past what the knobs reach; a width
// rewrites the rig's filter memory, so the first change keeps what the rig
// had, for Revert and for quitting.
//
// The change (ear::FilterEdit) waits until the rig is quiet: never while
// keyed and never while a burst is heard. Then it is on trial for KEEP_MS,
// the clock beside Undo, and goes back unless the operator presses Keep.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_EAR_FILTER_CONTROL_H
#define RAGTIME_EAR_FILTER_CONTROL_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/radio/RigFilter.h"

namespace ragtime
{
namespace ear
{

using radio::RigFilter;

// A passband in audio Hz.
struct Span
{
    double lowHz = 0;
    double highHz = 0;

    double width() const { return highHz - lowHz; }
    double centre() const { return (lowHz + highHz) / 2; }
};

// What Calibrate learned of a rig's twin PBT, for one mode and width; or,
// not calibrated, the centre and width the Ear goes by.
struct KnobModel
{
    double hzPerKnob = 0;   // an edge's move for a full turn; 0 not known
    double centreHz = 0;    // the passband's centre with the knobs centred
    double widthHz = 0;     // its width then, at -6 dB
    double nominalHz = 0;   // the width the rig said then
    double floorHz = 0;     // a cut in the audio hides edges below this; 0 none
    double ceilingHz = 0;   // and above this; 0 none

    bool known() const { return hzPerKnob != 0; }
    // Not calibrated: the edges move about this centre, the rig's width.
    static KnobModel about(double centreHz, const RigFilter& f);

    // "3000 1500 3140 3000 70 0", in the C locale, for the settings file;
    // and back (nothing from anything else, an older single number too).
    std::string text() const;
    static std::optional<KnobModel> fromText(const std::string& text);
};

// A knob's shift in Hz, and back.
double knobShift(std::optional<double> knob, double hzPerKnob);
// The passband a setting gives: the filter's own, past any cut in the
// audio; visible() is what the noise will show of it.
Span spanOf(const RigFilter& f, const KnobModel& m);
Span visible(const Span& s, const KnobModel& m);
Span spanOf(const RigFilter& f, double centreHz, double hzPerKnob);
// The centre with the knobs centred, from a passband measured with f set.
double centreOf(const Span& measured, const RigFilter& f, double hzPerKnob);
// The passband to set for a drag of the edges the noise shows from `from`
// to `to`: each edge moves as far as it was dragged from where the model
// puts it, so an edge hidden under a cut, or measured a little off the
// model, stays where it is when only the other is dragged.
Span dragTarget(const Span& from, const Span& to, const RigFilter& now, const KnobModel& m);
// A width the rig takes: 50 Hz steps to 500 Hz, then 100 Hz (an Icom's
// steps; other rigs round as they will), at least 50 Hz.
int roundWidth(double hz, bool up);
// The setting for the passband wanted, from the setting now: the knobs
// where they reach (hzPerKnob known and the rig has them), the width for
// the rest. The mode stays.
RigFilter settingFor(const Span& wanted, const RigFilter& now, const KnobModel& m);
RigFilter settingFor(const Span& wanted, const RigFilter& now, double centreHz, double hzPerKnob);
// Both knobs turned alike: the passband moves by hz and keeps its width,
// stopping where either knob would run out. Needs both knobs; the rest of
// the setting stays.
RigFilter shiftedBy(const RigFilter& now, double hz, double hzPerKnob);
// Hz per knob before Calibrate has measured it: the filter's width, as Jeff's
// IC-7100 showed (one full turn moves an edge one width). Only a guess,
// so a shift made with it may land a little off; the edges in the noise
// show where it went.
double guessHzPerKnob(const RigFilter& f);
// Whether a passband cuts into Data2G's (the wide modes', or the 500 Hz
// ones' with the 500 Hz switch on), by more than 10 Hz.
bool cutsData2G(const Span& span, bool narrow);

// "High edge 2700 → 2450 Hz", "Low edge 300 → 500 Hz, high edge
// 2700 → 2500 Hz", "Shifted 150 Hz up", "Width 2400 → 1800 Hz".
std::string changeText(const Span& from, const Span& to);

// A change in progress, and what the rig had.
class FilterEdit
{
public:
    static constexpr uint64_t KEEP_MS = 15000;

    enum class State
    {
        Idle,       // nothing waiting or on trial
        Waiting,    // a setting to write, once the rig is quiet
        Trial,      // written; goes back at the deadline unless kept
    };

    // The rig's filter as read. The first is what the rig had. A new mode
    // (the operator's: Ragtime never sets one) starts afresh, dropping a
    // change waiting or on trial and what the rig had in the last mode,
    // since putting either back would change the mode back too.
    void seen(const RigFilter& f);
    // The operator dragged or recalled: write to once quiet, then on trial.
    // What Undo goes back to stays what the rig had before the first
    // change since the last Keep.
    void change(const RigFilter& to, uint64_t nowMs);
    // Keep the trial; the clock stops.
    void keep();
    // Back to what the rig had before the change (once quiet).
    void undo();
    // Back to what the rig had when the Ear first read it.
    void putBack();

    // What to write now, if anything: a waiting setting once quiet (the
    // caller's word: not keyed, no burst being heard); at the deadline the
    // trial goes back.
    std::optional<RigFilter> due(uint64_t nowMs, bool quiet);

    // The Ear is closing: what to write so an unkept trial goes back.
    std::optional<RigFilter> close();
    // Ragtime is quitting: what to write to leave the rig as it was found,
    // if Ragtime ever changed it.
    std::optional<RigFilter> quit() const;

    State state() const { return state_; }
    bool reverting() const { return state_ == State::Waiting && revert_; }
    // Seconds left on the clock, while on trial.
    double secondsLeft(uint64_t nowMs) const;
    const std::optional<RigFilter>& found() const { return found_; }
    const std::optional<RigFilter>& current() const { return current_; }
    // The trial or waiting setting.
    const std::optional<RigFilter>& pending() const { return pending_; }
    // What Undo goes back to.
    const std::optional<RigFilter>& before() const { return before_; }
    // Ragtime has changed the rig since it found it.
    bool changed() const { return everWritten_; }

private:
    State state_ = State::Idle;
    std::optional<RigFilter> found_;
    std::optional<RigFilter> current_;
    std::optional<RigFilter> before_;
    std::optional<RigFilter> pending_;
    bool revert_ = false;       // the waiting setting goes back, no trial
    bool dirty_ = false;        // written since before_
    bool everWritten_ = false;
    uint64_t deadlineMs_ = 0;
};

// Calibrate: how far a knob's turn moves an edge, measured from the noise.
// It measures the passband with both knobs centred, then with PBT_IN up by
// STEP and by twice STEP, then PBT_IN back and PBT_OUT down by STEP and by
// twice STEP, then puts the setting back. Each pair of turns moves one edge
// (the low one up, or the high one down if the knobs run the other way),
// and the move between the two of a pair gives Hz per knob: two turns from
// one side and not one from the centre, since an edge at the centre can
// sit hidden under a cut in the audio (Jeff's IC-7100: its filter's low
// edge is near -70 Hz, under a cut at 70 Hz, so a single turn up from the
// centre showed two thirds of the move; measured 2026-10-26). The two
// pairs must agree. From the same measures come the passband's real centre
// and width with the knobs centred, and any cut that hides an edge. The
// caller writes what setting() says, waits for the rig to read it back
// and the noise to settle, and hands in the passband measured.
class Calibration
{
public:
    static constexpr double STEP = 0.15;
    // The two turns must agree this well.
    static constexpr double AGREE = 0.3;
    // An edge moving less than this is no measure.
    static constexpr double LEAST_HZ = 40;

    // Steps 0 to MEASURES - 1 measure; PUT_BACK puts the setting back.
    static constexpr int MEASURES = 5;
    static constexpr int PUT_BACK = MEASURES;

    // Starts from the setting now, which must have both knobs.
    bool start(const RigFilter& now);
    void cancel();
    bool running() const { return step_ >= 0 && step_ <= PUT_BACK; }
    int step() const { return step_; }

    // The setting to write for this step, while running.
    std::optional<RigFilter> setting() const;
    // The passband measured with it. The last step (putting back) needs no
    // measure: pass anything.
    void measured(const Span& span);

    // Once done: the model (Hz per knob signed: negative when a knob runs
    // the other way), or why there is none.
    const std::optional<KnobModel>& result() const { return result_; }
    const std::string& problem() const { return problem_; }
    // "Measuring with the knobs centred (1 of 5)".
    std::string stepText() const;

private:
    RigFilter original_;
    int step_ = -1;
    std::vector<Span> spans_;
    std::optional<KnobModel> result_;
    std::string problem_;
};

} // namespace ear
} // namespace ragtime

#endif
