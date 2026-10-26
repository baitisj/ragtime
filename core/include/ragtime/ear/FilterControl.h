// FilterControl: the Ear setting the rig's receive filter (docs/STAGE1.md,
// "The Ear", step 3). Only with the rig opened with Hamlib and "Let the Ear
// change the rig's filters" ticked (Settings > Rig, off by default); never
// through rigctld, which is shared.
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
// the noise (ear::Calibration). Until then only the width can be set, and
// the edges move together about the centre. Widening past the width sets
// a wider width, as does narrowing past what the knobs reach; a width
// rewrites the rig's filter memory, so the first change keeps what the rig
// had, for "Put back as I found them" and for quitting.
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

// A knob's shift in Hz, and back.
double knobShift(std::optional<double> knob, double hzPerKnob);
// The passband a setting gives.
Span spanOf(const RigFilter& f, double centreHz, double hzPerKnob);
// The centre with the knobs centred, from a passband measured with f set.
double centreOf(const Span& measured, const RigFilter& f, double hzPerKnob);
// A width the rig takes: 50 Hz steps to 500 Hz, then 100 Hz (an Icom's
// steps; other rigs round as they will), at least 50 Hz.
int roundWidth(double hz, bool up);
// The setting for the passband wanted, from the setting now: the knobs
// where they reach (hzPerKnob known and the rig has them), the width for
// the rest. The mode stays.
RigFilter settingFor(const Span& wanted, const RigFilter& now, double centreHz, double hzPerKnob);
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

    // The rig's filter as read. The first is what the rig had.
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
// From the setting now it centres both knobs and measures the passband,
// turns PBT_IN up by STEP and measures again, then PBT_IN back and PBT_OUT
// down by STEP, and measures a third time, then puts the setting back.
// Each turn shifts one filter, so an edge moves by hzPerKnob * STEP (the
// low edge up, or the high edge down if the knob runs the other way), and
// the two turns are averaged. The caller writes what next() says, waits
// for the rig to read it back and the noise to settle, and hands in the
// passband measured.
class Calibration
{
public:
    static constexpr double STEP = 0.15;
    // The two turns must agree this well.
    static constexpr double AGREE = 0.3;
    // An edge moving less than this is no measure.
    static constexpr double LEAST_HZ = 40;

    // Starts from the setting now, which must have both knobs.
    bool start(const RigFilter& now);
    void cancel();
    bool running() const { return step_ >= 0 && step_ < 4; }
    // 0 to 2 measure; 3 puts the setting back.
    int step() const { return step_; }

    // The setting to write for this step, while running.
    std::optional<RigFilter> setting() const;
    // The passband measured with it. The last step (putting back) needs no
    // measure: pass anything.
    void measured(const Span& span);

    // Once done: Hz per knob (signed: negative when a knob runs the other
    // way), or why there is none.
    std::optional<double> result() const { return result_; }
    const std::string& problem() const { return problem_; }
    // "Measuring with the knobs centred (1 of 3)".
    std::string stepText() const;

private:
    RigFilter original_;
    int step_ = -1;
    std::vector<Span> spans_;
    std::optional<double> result_;
    std::string problem_;
};

} // namespace ear
} // namespace ragtime

#endif
