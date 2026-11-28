// FilterControl: the Ear setting the rig's receive filter. Only with "Let the
// Ear change the rig's filters" ticked (Settings > Rig, off by default), with
// the rig opened with Hamlib or through rigctld (Jeff's OK, 2026-10-26:
// rigctld is shared, so the box is the operator's say-so).
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
// (the rig's width is nominal: an IC-7100 passes about 3140 Hz at
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
// IF SHIFT (2026-11-26, for an FT-891 and its Yaesu cousins): a rig with
// no PBT but IF SHIFT moves the whole passband by the shift, so
//
//     low  = centreHz - width / 2 + k * shift
//     high = centreHz + width / 2 + k * shift
//
// with k about 1 (or -1 if the rig shifts the other way in a mode), kept
// in KnobModel's hzPerKnob. Dragging the middle sets the shift; an edge
// alone sets the width and the shift together, so the other edge stays,
// each landing on the rig's own steps (its widths, 20 Hz of shift). Until
// Calibrate has measured k and the real centre, as for PBT, only the
// width and a shift taken as set.
//
// The notch (Jeff, 2026-10-26). Where the rig has a manual notch (Hamlib's
// MN), its bar is dragged into the spectrum to turn it on there, and back
// to its bay (NF OFF) to turn it off. An Icom places it with NOTCHF_RAW,
// a position, 0 to 1, not Hz: ear::NotchModel turns one into the other,
// starting from an IC-7100 and learning from where the dip shows in
// the noise (EdgeFinder::dip). Yaesu and Kenwood rigs have no NOTCHF_RAW
// and take NOTCHF, the centre in Hz, which needs no model (2026-11-26:
// an FT-891's notch went on wherever its own knob had left it, as only
// NOTCHF_RAW was ever set).
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
    double hzPerKnob = 0;   // an edge's move for a full turn (for IF SHIFT,
                            // per Hz of shift); 0 not known
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

// Where the notch's position puts it in the audio. Measured on Jeff's
// IC-7100 in PKTUSB with a 3000 Hz filter (2026-10-26): the dip's centre
// is about 5010 Hz x NOTCHF_RAW - 990 Hz (0.25 at 262 Hz, 0.50 at 1473,
// 0.75 at 2777), about 340 Hz wide at -6 dB and 42 to 45 dB deep; 0 and 1
// lie outside the passband. Other rigs start from the same line, and each
// dip measured corrects it: one moves the line, a second far enough from
// the first sets its slope too. Kept for each rig and mode.
//
// Its width too, from the dip: one for each of the rig's width steps (or
// the one width of a rig with none), so the Ear draws the notch as wide
// as it is and a drag of its side lands on the step nearest.
struct NotchModel
{
    static constexpr double HZ_PER_UNIT = 5010;
    static constexpr double OFFSET_HZ = -990;
    static constexpr double WIDTH_HZ = 340;
    // Two dips this far apart in position give the slope.
    static constexpr double SLOPE_SPAN = 0.1;

    double hzPerUnit = HZ_PER_UNIT;
    double offsetHz = OFFSET_HZ;
    // The last dip measured.
    std::optional<double> seenRaw;
    std::optional<double> seenHz;

    double hzOf(double raw) const { return hzPerUnit * raw + offsetHz; }
    // The position for a centre, in the rig's steps, 0 to 1.
    double rawFor(double hz) const;
    // A dip found at hz with the notch at raw.
    void learn(double raw, double hz);
    // Whether it has learned from a dip.
    bool learned() const { return seenRaw.has_value(); }

    // Widths measured, Hz, by step (the first for a rig with no steps);
    // 0 for one not yet measured.
    std::vector<double> widthsHz;
    // A dip's width measured at a step; true when what is kept moved.
    bool learnWidth(std::optional<int> step, double hz);
    // The width at a step of this many (none, or 0 steps: the rig's one
    // width): as measured, else from the nearest step measured, taking
    // each step as 1.5 times as wide as the one before; else WIDTH_HZ
    // with no steps, or 200 to 400 Hz across them.
    double widthAt(std::optional<int> step, int steps) const;
    // The step, of this many, whose width is nearest hz.
    int stepFor(double hz, int steps) const;
    // Its name: by how wide it is among the steps, "Narrow" and "Wide",
    // or "Narrow", "Mid" and "Wide"; "width 2" with more; empty with none.
    std::string stepName(std::optional<int> step, int steps) const;

    // "5010.0 -990.0 0.6824 2430.0", the last two a dash for none, then
    // the widths, a dash for one not measured, in the C locale; and back.
    std::string text() const;
    static std::optional<NotchModel> fromText(const std::string& text);
};

// Where a setting's notch is centred in the audio: the rig's own Hz
// (NOTCHF), else its position (NOTCHF_RAW) through the model; none
// from a rig that gives neither.
std::optional<double> notchCentre(const RigFilter& f, const NotchModel& m);
// Whether a notch at hz, this wide, cuts into Data2G's passband.
bool notchCutsData2G(double hz, bool narrow, double widthHz = NotchModel::WIDTH_HZ);
// A setting's notch width, Hz: its step's (or the rig's one width).
double notchWidthOf(const RigFilter& f, const NotchModel& m);
// The notch's width at a step, the rest as it is.
RigFilter withNotchWidth(const RigFilter& now, int step);
// The notch's change, or empty: "Notch on at 1500 Hz", "Notch 1500 →
// 1200 Hz", "Notch off", "Notch Narrow → Wide".
std::string notchChangeText(const RigFilter& from, const RigFilter& to, const NotchModel& m);
// The notch on, centred at hz (off with none), the rest as it is: in Hz,
// to the nearest 10, for a rig that gives its notch in Hz and no
// position, else as a position.
RigFilter withNotch(const RigFilter& now, std::optional<double> hz, const NotchModel& m);

// How the rig moves its passband: twin PBT (both knobs read), or IF SHIFT
// on a rig without them.
bool twinPbt(const RigFilter& f);
bool ifShifts(const RigFilter& f);
inline bool shifts(const RigFilter& f) { return twinPbt(f) || ifShifts(f); }
// IF SHIFT's reach either side (an FT-891's and most Yaesus'), and its step.
constexpr int IF_SHIFT_MAX_HZ = 1200;
constexpr int IF_SHIFT_STEP_HZ = 20;
// The width a calibration is kept under: the rig's for twin PBT (the
// knobs' reach goes with it), 0 for IF SHIFT, whose model holds at every
// width (an edge dragged alone changes the width each time).
int calibrationWidth(const RigFilter& f);
// The widths a rig's filter takes in a mode, narrowest first, where
// Ragtime knows them: an FT-891's ("hamlib-1036", as Hamlib 4.7 sets
// them: up to 3200 Hz in SSB, to 3000 in the data, CW and RTTY modes).
// Empty for the rest, which round as an Icom does (roundWidth).
std::vector<int> widthSteps(const std::string& rigKey, const std::string& mode);
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
// puts it, so an edge measured a little off the model stays where it is
// when only the other is dragged. An edge the model puts under a cut in
// the audio shows at the cut, not where it is, so dragged by itself it
// goes where it was dropped (2026-10-26: an IC-7100's low edge at -72 Hz,
// shown at 70, dropped at 400 landed at 280 when moved by the drag).
Span dragTarget(const Span& from, const Span& to, const RigFilter& now, const KnobModel& m);
// Where a drag from `from` (the edges as shown) lands once `to` replaces
// `now`, as the scope will show it: what the model gives `to`, past any
// cut in the audio, off by as much as the edges shown were off the model.
// Not where it was dropped: past the knobs' ends a drag can't go all the
// way (Jeff, 2026-10-26: the top edge dropped at 5 kHz, the knobs
// stopping it near 4.4), and the next drag's move, taken from there, left
// the passband several hundred Hz short.
Span reached(const Span& from, const RigFilter& now, const RigFilter& to, const KnobModel& m);
// A width the rig takes: one of its steps where they are known (up, the
// narrowest at least hz; else the nearest), else 50 Hz steps to 500 Hz,
// then 100 Hz (an Icom's steps; other rigs round as they will), at least
// 50 Hz.
int roundWidth(double hz, bool up, const std::vector<int>& steps = {});
// The setting for the passband wanted, from the setting now: the knobs
// where they reach (hzPerKnob known and the rig has them), the width for
// the rest; with IF SHIFT, the width nearest the one wanted and the shift
// that centres it. The mode stays.
RigFilter settingFor(const Span& wanted, const RigFilter& now, const KnobModel& m,
                     const std::vector<int>& steps = {});
RigFilter settingFor(const Span& wanted, const RigFilter& now, double centreHz, double hzPerKnob);
// Both knobs turned alike: the passband moves by hz and keeps its width,
// stopping where either knob would run out; or IF SHIFT moved by hz / k,
// within its reach. Needs both knobs or IF SHIFT; the rest of the setting
// stays.
RigFilter shiftedBy(const RigFilter& now, double hz, double hzPerKnob);
// Hz per knob before Calibrate has measured it: the filter's width, as Jeff's
// IC-7100 showed (one full turn moves an edge one width). Only a guess,
// so a shift made with it may land a little off; the edges in the noise
// show where it went.
double guessHzPerKnob(const RigFilter& f);
// Disable (Jeff, 2026-10-26): the setting with the PBT (or IF SHIFT)
// centred and the notch off, the mode and width as they are; and whether a setting has
// them so (a knob the rig hasn't counts as centred, a notch it hasn't as
// off).
RigFilter disabled(const RigFilter& now);
bool filtersOff(const RigFilter& f);
// The rig has what a preset holds: the same mode and width, the knobs
// and the notch within one and a half of the rig's steps, and the same
// notch width step. Parts the preset doesn't hold match anything.
bool holds(const RigFilter& rig, const RigFilter& preset);
// Whether a passband cuts more than 10 Hz into what of Data2G's it must
// leave whole (data2gKept: the wide modes', or with the 500 Hz switch on
// the narrowest ones' 200 Hz in the middle).
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
    // Keep the trial; the clock stops. Pressed while a change still waits
    // for a quiet moment, it is kept as it is written, with no trial (Jeff,
    // 2026-10-26: Keep pressed then, greyed, did nothing, and the change
    // went back 15 s after it was written).
    void keep();
    // Keep was pressed for the change waiting.
    bool keptAhead() const { return state_ == State::Waiting && !revert_ && keepAhead_; }
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
    // The clock stands still while held (the pointer on Undo or Keep, so
    // reaching for Keep never loses the change).
    void hold(bool held, uint64_t nowMs);
    bool held() const { return held_; }
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
    bool keepAhead_ = false;    // the waiting setting is kept once written
    bool dirty_ = false;        // written since before_
    bool everWritten_ = false;
    uint64_t deadlineMs_ = 0;
    bool held_ = false;
    uint64_t heldLeftMs_ = 0;       // left on the clock when held
};

// Calibrate: how far a knob's turn moves an edge, measured from the noise.
// With IF SHIFT instead, the same five measures at a shift of 0, +200,
// +400, -200 and -400 Hz, each pair moving both edges: the edge clear of
// any cut gives k.
// It measures the passband with both knobs centred, then with PBT_IN up by
// STEP and by twice STEP, then PBT_IN back and PBT_OUT down by STEP and by
// twice STEP, then puts the setting back. Each pair of turns moves one edge
// (the low one up, or the high one down if the knobs run the other way),
// and the move between the two of a pair gives Hz per knob: two turns from
// one side and not one from the centre, since an edge at the centre can
// sit hidden under a cut in the audio (an IC-7100: its filter's low
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
    static constexpr int IF_STEP_HZ = 200;
    // A shift moving the passband by less or more than this, per Hz, is no
    // IF SHIFT.
    static constexpr double IF_LEAST = 0.5;
    static constexpr double IF_MOST = 2.0;
    // The two turns must agree this well.
    static constexpr double AGREE = 0.3;
    // An edge moving less than this is no measure.
    static constexpr double LEAST_HZ = 40;

    // Steps 0 to MEASURES - 1 measure; PUT_BACK puts the setting back.
    static constexpr int MEASURES = 5;
    static constexpr int PUT_BACK = MEASURES;

    // Starts from the setting now, which must have both knobs or IF SHIFT.
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
    bool ifShift_ = false;
    int step_ = -1;
    std::vector<Span> spans_;
    std::optional<KnobModel> result_;
    std::string problem_;
};

} // namespace ear
} // namespace ragtime

#endif
