// The microphone deck's knobs (docs/STAGE1.md, "The microphone deck"),
// with the feel of Glissando's Dial (Jeff, 2026-10-31; ported from
// Glissando's ChaoticaControls.cpp, written by Claude and Jeff, under MIT):
//
//   - Drag up or down: a full sweep for 300 pixels, a tenth of that with
//     Shift held.
//   - The mouse wheel: one step a click, with its own step for Shift and
//     for Control. Part clicks from smooth wheels and touchpads are saved
//     up, so a touchpad turns it evenly.
//   - A detent at the default: dragging catches there for a few pixels,
//     so it can be found by hand, and a double click returns to it.
//   - Push-pull: a click that doesn't turn the knob pushes it in or pops
//     it out. A double click on one returns it to its default value and
//     its default push.
//   - New in Ragtime: keys, since the deck's knobs take focus. Up and
//     Right one wheel step (Shift fine), Down and Left back, Page Up and
//     Page Down a coarse step, Home and End the ends, Space or Enter a
//     push.
//
// A knob of positions (the high-pass: 80, 100, 150 Hz) clicks between
// them: dragging snaps to the nearest, and the wheel and keys move one
// position whatever is held.
//
// This is the knob's behaviour with no drawing in it, so it is tested
// here; app/src/Knob draws it.
#ifndef RAGTIME_DECK_KNOB_H
#define RAGTIME_DECK_KNOB_H

#include <cmath>
#include <vector>

namespace ragtime
{
namespace deck
{

// Pixels of travel for a full sweep, and how far it must move before a
// push-pull knob turns rather than pushes.
constexpr int KNOB_SWEEP_PIXELS = 300;
constexpr int KNOB_PUSH_SLOP_PIXELS = 3;
// The detent at the default, in pixels either side.
constexpr int KNOB_DETENT_PIXELS = 4;

struct KnobSpec
{
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.01;             // values are kept to a whole number of these
    double defaultValue = 0.0;
    // The value with the pointer straight up; NAN for halfway. Either
    // side of it the knob turns evenly, so it can spend more of its sweep
    // on one side.
    double centre = NAN;
    // A wheel click alone, with Shift and with Control. 0 for a hundredth
    // of the range, and one step with Shift.
    double wheelPlain = 0.0;
    double wheelShift = 0.0;
    double wheelControl = 0.0;
    // Only these values, in order (a rotary switch); empty for any.
    std::vector<double> positions;
    bool pushable = false;
    bool defaultPushed = false;
};

enum class KnobKey
{
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Push,       // Space or Enter
};

class Knob
{
public:
    explicit Knob(const KnobSpec& spec);

    const KnobSpec& spec() const { return spec_; }
    double value() const { return value_; }
    bool pushed() const { return pushed_; }
    bool dragging() const { return dragging_; }
    // Where the value sits along the sweep, 0 to 1, and the value at a
    // place along it (snapped to a position on a switch).
    double fraction() const { return fractionOf(value_); }
    double fractionOf(double value) const;
    double valueAt(double fraction) const;

    // Set from outside (a saved setting): no detent, no event. Returns
    // whether it changed.
    bool setValue(double value);
    bool setPushed(bool pushed);

    // The mouse. Each returns whether the value changed; release says
    // whether it pushed or popped the knob.
    void press(int y);
    bool drag(int y, bool fine);
    bool release(int y);
    bool wheel(int rotation, int delta, bool shift, bool control);
    // A double click: the default back, and on a push-pull knob its
    // default push too (the click before it pushed it, and the release
    // after it doesn't). Returns whether either changed.
    bool doubleClick();
    // A key: whether the value changed, or (Push) the knob was pushed.
    bool key(KnobKey key, bool shift);

private:
    double snap(double value) const;
    double wheelStep(bool shift, bool control) const;
    // A switch's position index for a value, and the value at one.
    int positionOf(double value) const;
    bool change(double value);

    KnobSpec spec_;
    double value_;
    bool pushed_;
    bool dragging_ = false;
    bool turned_ = false;
    bool swallowRelease_ = false;
    int dragY_ = 0;
    double dragValue_ = 0.0;
    int wheelRotation_ = 0;
};

} // namespace deck
} // namespace ragtime

#endif
