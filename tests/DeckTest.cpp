// The microphone deck's knobs, meters and push to talk (core/src/deck).
#include <cmath>

#include "Check.h"
#include "ragtime/deck/Knob.h"
#include "ragtime/deck/Meters.h"

using namespace ragtime::deck;

namespace
{

KnobSpec gainSpec()
{
    // The Gain knob: -20 to +40 dB, 0 by default, 1 dB a click, 0.1 with
    // Shift, 6 with Control.
    KnobSpec s;
    s.minimum = -20.0;
    s.maximum = 40.0;
    s.step = 0.1;
    s.defaultValue = 0.0;
    s.wheelPlain = 1.0;
    s.wheelShift = 0.1;
    s.wheelControl = 6.0;
    return s;
}

bool near(double a, double b, double e = 1e-6)
{
    return std::fabs(a - b) < e;
}

void dragging()
{
    Knob k(gainSpec());
    CHECK(near(k.value(), 0.0));
    // A full sweep (60 dB) for 300 px: 30 px up is +6 dB.
    k.press(100);
    CHECK(k.dragging());
    CHECK(k.drag(70, false));
    CHECK(near(k.value(), 6.0));
    // Past the top it stops at the top.
    k.drag(-1000, false);
    CHECK(near(k.value(), 40.0));
    k.release(-1000);
    CHECK(!k.dragging());

    // Shift: a tenth as far, so 30 px is 0.6 dB.
    k.setValue(10.0);
    k.press(100);
    k.drag(70, true);
    CHECK(near(k.value(), 10.6));
    k.release(70);

    // The detent: from +2 dB (10 px above 0), dragging down 8 px lands
    // within 4 px of the default and catches on 0; 20 px goes past.
    k.setValue(2.0);
    k.press(0);
    k.drag(8, false);
    CHECK(near(k.value(), 0.0));
    k.drag(20, false);
    CHECK(near(k.value(), -2.0));
    k.release(20);
    // Double click: back to the default.
    k.setValue(17.0);
    CHECK(k.doubleClick());
    CHECK(near(k.value(), 0.0));
    CHECK(!k.doubleClick());
}

void wheels()
{
    Knob k(gainSpec());
    // One click is 120 on most mice.
    CHECK(k.wheel(120, 120, false, false));
    CHECK(near(k.value(), 1.0));
    k.wheel(120, 120, true, false);
    CHECK(near(k.value(), 1.1));
    k.wheel(-120, 120, false, true);
    CHECK(near(k.value(), -4.9));
    // A touchpad's part clicks are saved up: three 40s are one click.
    CHECK(!k.wheel(40, 120, false, false));
    CHECK(!k.wheel(40, 120, false, false));
    CHECK(k.wheel(40, 120, false, false));
    CHECK(near(k.value(), -3.9));
    // Never past the ends.
    for (int i = 0; i < 20; i++) k.wheel(-120, 120, false, true);
    CHECK(near(k.value(), -20.0));
}

void keys()
{
    Knob k(gainSpec());
    CHECK(k.key(KnobKey::Up, false));
    CHECK(near(k.value(), 1.0));
    k.key(KnobKey::Right, true);
    CHECK(near(k.value(), 1.1));
    k.key(KnobKey::Down, false);
    k.key(KnobKey::Left, false);
    CHECK(near(k.value(), -0.9));
    k.key(KnobKey::PageUp, false);
    CHECK(near(k.value(), 5.1));
    k.key(KnobKey::PageDown, false);
    k.key(KnobKey::PageDown, false);
    CHECK(near(k.value(), -6.9));
    k.key(KnobKey::End, false);
    CHECK(near(k.value(), 40.0));
    k.key(KnobKey::Home, false);
    CHECK(near(k.value(), -20.0));
    // Not a push-pull knob: Space does nothing.
    CHECK(!k.key(KnobKey::Push, false));
    CHECK(!k.pushed());
}

void pushPull()
{
    // The Target knob: pushed in by default (peaks out), -26 dBFS.
    KnobSpec s;
    s.minimum = -36.0;
    s.maximum = -16.0;
    s.step = 0.5;
    s.defaultValue = -26.0;
    s.wheelPlain = 1.0;
    s.wheelShift = 0.5;
    s.wheelControl = 3.0;
    s.pushable = true;
    s.defaultPushed = true;
    Knob k(s);
    CHECK(k.pushed());
    // A click pops it out.
    k.press(50);
    CHECK(k.release(51));
    CHECK(!k.pushed());
    // A wobble under 3 px is still a click, and turns nothing.
    k.press(50);
    CHECK(!k.drag(52, false));
    CHECK(near(k.value(), -26.0));
    CHECK(k.release(52));
    CHECK(k.pushed());
    // A real turn is no push.
    k.press(50);
    CHECK(k.drag(20, false));
    CHECK(!k.release(20));
    CHECK(k.pushed());
    CHECK(near(k.value(), -24.0));
    // Pop it out and turn it, then double click: the first click of the
    // pair pushed it, the double click puts default value and push back,
    // and the release after it is no push.
    k.press(0);
    k.release(0);
    CHECK(!k.pushed());
    k.press(0);
    k.release(0);
    CHECK(k.pushed());
    CHECK(k.doubleClick());
    CHECK(near(k.value(), -26.0));
    CHECK(k.pushed());
    CHECK(!k.release(0));
    CHECK(k.pushed());
    // Space pushes.
    CHECK(k.key(KnobKey::Push, false));
    CHECK(!k.pushed());
    // With nothing to change, a double click says so.
    k.key(KnobKey::Push, false);
    CHECK(!k.doubleClick());
}

void positions()
{
    // The high-pass: 80, 100, 150 Hz, 100 in by default.
    KnobSpec s;
    s.positions = {150.0, 80.0, 100.0};
    s.defaultValue = 100.0;
    s.pushable = true;
    s.defaultPushed = true;
    Knob k(s);
    CHECK(near(k.value(), 100.0));
    CHECK(near(k.fraction(), 0.5));
    // A wheel click is one position, whatever is held.
    CHECK(k.wheel(120, 120, false, true));
    CHECK(near(k.value(), 150.0));
    CHECK(!k.wheel(120, 120, true, false));
    k.key(KnobKey::Down, false);
    k.key(KnobKey::Down, true);
    CHECK(near(k.value(), 80.0));
    CHECK(!k.key(KnobKey::Left, false));
    // Dragging snaps: 300 px is the whole way, so 120 px up from 80 is
    // past halfway to 100 and 250 px reaches 150.
    k.press(300);
    CHECK(k.drag(180, false));
    CHECK(near(k.value(), 100.0));
    k.drag(50, false);
    CHECK(near(k.value(), 150.0));
    k.release(50);
    // Any value set snaps to the nearest position.
    k.setValue(130.0);
    CHECK(near(k.value(), 150.0));
    k.setValue(91.0);
    CHECK(near(k.value(), 100.0));
}

void centre()
{
    // A knob that spends half its sweep below its centre.
    KnobSpec s;
    s.minimum = 0.0;
    s.maximum = 100.0;
    s.step = 1.0;
    s.centre = 20.0;
    Knob k(s);
    CHECK(near(k.fractionOf(20.0), 0.5));
    CHECK(near(k.fractionOf(10.0), 0.25));
    CHECK(near(k.fractionOf(60.0), 0.75));
    CHECK(near(k.valueAt(0.75), 60.0));
}

void meters()
{
    CHECK(near(faceFraction(-20.0, -40.0, 0.0), 0.5));
    CHECK(near(faceFraction(-60.0, -40.0, 0.0), 0.0));
    CHECK(near(faceFraction(3.0, -40.0, 0.0), 1.0));
    CHECK(near(faceFraction(NAN, -40.0, 0.0), 0.0));

    // A VU eases: after one time constant it is 63% of the way.
    VuNeedle vu(-40.0, 0.3);
    vu.step(-20.0, 0.3);
    CHECK(near(vu.position(), -40.0 + 20.0 * (1.0 - std::exp(-1.0)), 1e-9));
    for (int i = 0; i < 100; i++) vu.step(-20.0, 0.033);
    CHECK(near(vu.position(), -20.0, 0.01));
    // Silence takes it back to the floor, never under.
    for (int i = 0; i < 300; i++) vu.step(-120.0, 0.033);
    CHECK(vu.position() >= -40.0 && vu.position() < -39.9);

    // Loudest: jumps up at once, falls 24 dB a second; the hold waits a
    // second then falls 10 dB a second.
    PeakNeedle p(-30.0);
    p.step(-12.0, 0.033);
    CHECK(near(p.position(), -12.0));
    CHECK(near(p.hold(), -12.0));
    p.step(-30.0, 0.5);
    CHECK(near(p.position(), -24.0));
    CHECK(near(p.hold(), -12.0));
    p.step(-30.0, 0.5);
    CHECK(near(p.position(), -30.0));
    CHECK(near(p.hold(), -12.0));
    p.step(-30.0, 1.0);
    CHECK(near(p.hold(), -22.0));
    p.step(-30.0, 2.0);
    CHECK(near(p.hold(), -30.0));
    // A new peak lifts the hold and restarts its wait.
    p.step(-15.0, 0.1);
    p.step(-28.0, 0.9);
    CHECK(near(p.hold(), -15.0));
    p.reset();
    CHECK(near(p.hold(), -30.0));

    CHECK_EQ(dbText(-26.6, -40.0), std::string("\xE2\x88\x92" "27"));
    CHECK_EQ(dbText(-50.0, -40.0), std::string("\xE2\x80\x94"));
    CHECK_EQ(dbText(0.2, -40.0), std::string("0"));
}

void talk()
{
    Talk t;
    CHECK(t.phase() == TalkPhase::Idle);
    CHECK(!t.canMonitor(false));
    CHECK(t.startMonitor(true));
    CHECK(t.monitoring());
    // Push to talk stops Monitor, and says so.
    CHECK(t.press());
    CHECK(!t.monitoring());
    CHECK(t.phase() == TalkPhase::Recording);
    // Monitor is greyed while held.
    CHECK(!t.canMonitor(true));
    CHECK(!t.startMonitor(true));
    CHECK(!t.press());
    CHECK(t.release());
    CHECK(t.phase() == TalkPhase::Coding);
    CHECK(!t.canMonitor(true));
    t.coded(false, false);
    CHECK(t.phase() == TalkPhase::Idle);

    // Esc while held.
    t.press();
    CHECK(t.discard());
    CHECK(t.phase() == TalkPhase::Idle);
    CHECK(!t.release());

    // Held for review: Monitor plays it, Send or Discard ends it, and a
    // new press throws it away.
    t.press();
    t.release();
    t.coded(true, false);
    CHECK(t.phase() == TalkPhase::Review);
    CHECK(t.canMonitor(true));
    CHECK(t.sendReviewed());
    CHECK(t.phase() == TalkPhase::Idle);
    CHECK(!t.sendReviewed());
    t.press();
    t.release();
    t.coded(true, false);
    CHECK(!t.press());
    CHECK(t.phase() == TalkPhase::Recording);
    t.release();
    // Nothing but silence: nothing to review.
    t.coded(true, true);
    CHECK(t.phase() == TalkPhase::Idle);
}

void airBar()
{
    AirBar a;
    a.airSeconds = 21.0;
    a.speechSeconds = 14.2;
    CHECK(!a.over());
    CHECK(near(a.scaleSeconds(), 60.0));
    CHECK_EQ(a.words(), std::string("0:21 of air for 0:14 of speech"));
    a.airSeconds = 70.0;
    CHECK(a.over());
    CHECK(a.scaleSeconds() >= 70.0);
    CHECK_EQ(clockText(750.0), std::string("12:30"));
    CHECK_EQ(clockText(-3.0), std::string("0:00"));
}

} // namespace

int main()
{
    dragging();
    wheels();
    keys();
    pushPull();
    positions();
    centre();
    meters();
    talk();
    airBar();
    return ragtime_test::finish();
}
