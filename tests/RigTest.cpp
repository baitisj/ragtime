// Rig control without a rig: the settings as Hamlib's configuration words,
// the words kept in the settings file, and the PTT follower's keying and
// its safety unkeys, the transmit meters burst by burst, and Auto ALC.
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "Check.h"
#include "ragtime/radio/AutoAlc.h"
#include "ragtime/radio/PttFollower.h"
#include "ragtime/radio/RigSettings.h"
#include "ragtime/radio/TxMeters.h"

using namespace ragtime::radio;

namespace
{

using Conf = std::vector<std::pair<std::string, std::string>>;

std::string joined(const Conf& conf)
{
    std::string text;
    for (const auto& [name, value] : conf) text += (text.empty() ? "" : " ") + name + "=" + value;
    return text;
}

void backendConf()
{
    Conf out;
    std::string problem;
    CHECK(parseBackendConf("civaddr=0x88  timeout=500,retry=3;\tauto_power_on=1\n", out, problem));
    CHECK_EQ(joined(out), std::string("civaddr=0x88 timeout=500 retry=3 auto_power_on=1"));
    CHECK(parseBackendConf("", out, problem) && out.empty());
    CHECK(parseBackendConf("  ,; ", out, problem) && out.empty());
    // An empty value is still a setting.
    CHECK(parseBackendConf("rig_pathname=", out, problem) && out.size() == 1 && out[0].second.empty());
    CHECK(!parseBackendConf("civaddr 0x88", out, problem));
    CHECK(out.empty());
    CHECK(problem.find("\"civaddr\"") != std::string::npos);
    CHECK(!parseBackendConf("=5", out, problem));
}

void hamlibWords()
{
    RigSettings s;
    s.source = RigSource::Hamlib;
    s.model = 3070;
    std::string problem;
    // No port: nothing to open.
    CHECK(hamlibConf(s, PortType::Serial, problem).empty());
    CHECK(!problem.empty());

    s.port = "/dev/ttyUSB0";
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)), std::string("rig_pathname=/dev/ttyUSB0"));
    CHECK(problem.empty());

    s.baud = 19200;
    s.dataBits = 8;
    s.stopBits = 1;
    s.parity = Parity::Even;
    s.handshake = Handshake::Hardware;
    s.dtr = LineState::On;
    s.rts = LineState::Off;
    s.backendConf = "civaddr=0x88";
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)),
             std::string("rig_pathname=/dev/ttyUSB0 serial_speed=19200 data_bits=8 stop_bits=1 serial_parity=Even "
                         "serial_handshake=Hardware dtr_state=ON rts_state=OFF civaddr=0x88"));
    // A network rig takes no serial settings.
    s.port = "192.168.1.20:4992";
    CHECK_EQ(joined(hamlibConf(s, PortType::Network, problem)), std::string("rig_pathname=192.168.1.20:4992 civaddr=0x88"));

    // CAT keying is the backend's own; DTR and RTS name the line and port.
    s.port = "/dev/ttyUSB0";
    s.baud = 0;
    s.dataBits = s.stopBits = 0;
    s.parity = Parity::Default;
    s.handshake = Handshake::Default;
    s.dtr = s.rts = LineState::Default;
    s.backendConf.clear();
    s.keyRig = true;
    s.ptt = PttMethod::CatData;
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)),
             std::string("rig_pathname=/dev/ttyUSB0 ptt_type=RIGMICDATA"));
    s.ptt = PttMethod::CatMic;
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)), std::string("rig_pathname=/dev/ttyUSB0 ptt_type=RIG"));
    s.ptt = PttMethod::Rts;
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)),
             std::string("rig_pathname=/dev/ttyUSB0 ptt_type=RTS ptt_pathname=/dev/ttyUSB0"));
    s.ptt = PttMethod::Dtr;
    s.pttPort = "/dev/ttyUSB1";
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)),
             std::string("rig_pathname=/dev/ttyUSB0 ptt_type=DTR ptt_pathname=/dev/ttyUSB1"));
    // Not keying: the line is left alone.
    s.keyRig = false;
    CHECK_EQ(joined(hamlibConf(s, PortType::Serial, problem)), std::string("rig_pathname=/dev/ttyUSB0"));

    // Hamlib's dummy rig needs no port; keying it by a line does.
    s.port.clear();
    CHECK(hamlibConf(s, PortType::None, problem).empty() && problem.empty());
    s.keyRig = true;
    s.pttPort.clear();
    CHECK(hamlibConf(s, PortType::None, problem).empty());
    CHECK(problem.find("DTR") != std::string::npos);
    s.pttPort = "/dev/ttyUSB1";
    CHECK_EQ(joined(hamlibConf(s, PortType::None, problem)), std::string("ptt_type=DTR ptt_pathname=/dev/ttyUSB1"));
    s.keyRig = false;
    CHECK(hamlibConf(s, PortType::Network, problem).empty());
    CHECK(problem.find("host:port") != std::string::npos);
    s.port = "/dev/ttyUSB0";

    s.backendConf = "civaddr";
    CHECK(hamlibConf(s, PortType::Serial, problem).empty());
    CHECK(problem.find("civaddr") != std::string::npos);
}

void words()
{
    CHECK_EQ(std::string(sourceWord(RigSource::Hamlib)), std::string("hamlib"));
    CHECK(sourceFromWord("rigctld") == RigSource::Rigctld);
    CHECK(sourceFromWord("nonsense") == RigSource::None);
    for (PttMethod p : {PttMethod::CatData, PttMethod::CatMic, PttMethod::Dtr, PttMethod::Rts})
        CHECK(pttFromWord(pttWord(p)) == p);
    CHECK(pttFromWord("") == PttMethod::CatData);
    for (Parity p : {Parity::Default, Parity::None, Parity::Odd, Parity::Even, Parity::Mark, Parity::Space})
        CHECK(parityFromWord(parityWord(p)) == p);
    CHECK(handshakeFromWord("XONXOFF") == Handshake::XonXoff);
    CHECK(handshakeFromWord("") == Handshake::Default);
    CHECK(lineFromWord("OFF") == LineState::Off);
    CHECK_EQ(std::string(lineWord(LineState::On)), std::string("ON"));
}

void sameRigs()
{
    RigSettings a;
    a.source = RigSource::Hamlib;
    a.model = 3070;
    a.port = "/dev/ttyUSB0";
    RigSettings b = a;
    CHECK(sameRig(a, b));
    // The poll interval applies to an open rig, and the PTT method and port
    // matter only while keying.
    b.pollSeconds = 5;
    b.ptt = PttMethod::Rts;
    b.pttPort = "/dev/ttyUSB1";
    CHECK(sameRig(a, b));
    b.keyRig = true;
    CHECK(!sameRig(a, b));
    a.keyRig = true;
    a.ptt = PttMethod::Rts;
    a.pttPort = "/dev/ttyUSB1";
    CHECK(sameRig(a, b));
    b.ptt = PttMethod::CatData;
    CHECK(!sameRig(a, b));
    b = a;
    b.baud = 9600;
    CHECK(!sameRig(a, b));
    b = a;
    b.backendConf = "civaddr=0x88";
    CHECK(!sameRig(a, b));
}

const uint64_t T = 1000000;

void follows()
{
    PttFollower f;
    // Off by default: the host's PTT changes nothing.
    f.host(true, T);
    CHECK(!f.keyed());
    f.host(false, T);
    f.setEnabled(true);
    f.host(true, T);
    CHECK(f.keyed());
    uint64_t v = f.version();
    f.host(true, T + 10);
    CHECK_EQ(f.version(), v);
    f.host(false, T + 2000);
    CHECK(!f.keyed());
    CHECK(f.why().empty());
    CHECK(f.version() > v);

    // Turned off while keyed: unkeys.
    f.host(true, T + 3000);
    CHECK(f.keyed());
    f.setEnabled(false);
    CHECK(!f.keyed());
}

void safety()
{
    PttFollower f;
    f.setEnabled(true);

    // 180 s and still keyed: unkeyed, and held until PTT OFF.
    f.host(true, T);
    f.tick(T + PttFollower::MAX_KEYED_MS - 1);
    CHECK(f.keyed());
    f.tick(T + PttFollower::MAX_KEYED_MS);
    CHECK(!f.keyed());
    CHECK(f.why().find("180 s") != std::string::npos);
    f.host(true, T + PttFollower::MAX_KEYED_MS + 10);
    CHECK(!f.keyed());
    f.host(false, T + PttFollower::MAX_KEYED_MS + 20);
    CHECK(f.why().empty());
    f.host(true, T + PttFollower::MAX_KEYED_MS + 30);
    CHECK(f.keyed());
    // The cap counts from this keying.
    f.tick(T + PttFollower::MAX_KEYED_MS + 30 + 1000);
    CHECK(f.keyed());

    // The host link drops: unkeyed, and a host that comes back keys again
    // with no PTT OFF first.
    f.hostLost();
    CHECK(!f.keyed());
    CHECK(f.why().find("data2g-host") != std::string::npos);
    f.host(true, T + 400000);
    CHECK(f.keyed());

    // The rig fails: unkeyed and held.
    f.rigFailed("Hamlib: communication timed out");
    CHECK(!f.keyed());
    CHECK_EQ(f.why(), std::string("Hamlib: communication timed out"));
    f.host(true, T + 400100);
    CHECK(!f.keyed());
    f.host(false, T + 400200);
    f.host(true, T + 400300);
    CHECK(f.keyed());
    // Unkeyed already: a failure changes nothing.
    f.host(false, T + 400400);
    f.rigFailed("again");
    f.host(true, T + 400500);
    CHECK(f.keyed());
}

MeterReading alcOnly(double alc)
{
    MeterReading r;
    r.alc = alc;
    return r;
}

void meters()
{
    TxMeters m;
    // Nothing counts between bursts.
    m.reading(alcOnly(0.9), 100);
    CHECK(!m.transmitting() && !m.latest().any() && !m.last());

    m.transmitting(true, 1000);
    MeterReading r;
    r.alc = 0.3;
    r.swr = 1.2;
    r.watts = 40;
    m.reading(r, 1250);
    CHECK_EQ(readingText(m.latest()), std::string("ALC 30% \xc2\xb7 SWR 1.2 \xc2\xb7 40 W"));
    // A reading with only some meters keeps the others.
    m.reading(alcOnly(0.6), 1500);
    CHECK_EQ(readingText(m.latest()), std::string("ALC 60% \xc2\xb7 SWR 1.2 \xc2\xb7 40 W"));
    MeterReading w;
    w.swr = 1.5;
    w.watts = 36;
    m.reading(w, 1750);
    m.reading(alcOnly(0.55), 2000);
    m.reading(alcOnly(0.6), 2250);
    m.reading(alcOnly(0.4), 2500);
    m.reading(alcOnly(0.6), 2750);
    // A reading far over at the end doesn't sway the median.
    m.reading(alcOnly(0.85), 3000);
    CHECK_EQ(m.current().alcStepDb, 0.0);
    m.transmitting(false, 13000);
    CHECK(!m.latest().any());
    CHECK_EQ(m.bursts(), (uint64_t)1);
    const TxMeters::Burst& b = *m.last();
    CHECK_EQ(b.readings, 8);
    CHECK(b.alcPeak && *b.alcPeak == 0.85);
    CHECK(b.alcMean && std::fabs(*b.alcMean - (0.3 + 0.6 + 0.55 + 0.6 + 0.4 + 0.6 + 0.85) / 7) < 1e-9);
    CHECK(b.swrPeak && *b.swrPeak == 1.5 && !b.swrHigh());
    CHECK(b.wattsMean && *b.wattsMean == 38);
    // Readings 0.3 0.4 0.55 0.6 0.6 0.6 0.85: the median 60%, in the band.
    CHECK(b.alcReadings == 7 && b.alcMedian && *b.alcMedian == 0.6);
    CHECK_EQ(b.alcStepDb, 0.0);
    CHECK(!b.overTarget());
    CHECK_EQ(burstText(b), std::string("12 s: ALC peak 85%, mean 56%, median 60% \xc2\xb7 SWR 1.5 \xc2\xb7 38 W"));

    // A burst with no readings (a rig that reports nothing) keeps the last.
    m.transmitting(true, 20000);
    m.transmitting(false, 21000);
    CHECK_EQ(m.bursts(), (uint64_t)1);
    CHECK_EQ(m.last()->startMs, (uint64_t)1000);

    // Power as a share when the backend has no watts; a short burst in
    // tenths; a high SWR.
    m.transmitting(true, 30000);
    MeterReading p;
    p.power = 0.31;
    p.swr = 3.4;
    m.reading(p, 30500);
    CHECK_EQ(readingText(m.latest()), std::string("SWR 3.4 \xc2\xb7 31% power"));
    m.transmitting(false, 32500);
    CHECK(m.last()->swrHigh() && !m.last()->overTarget());
    CHECK_EQ(burstText(*m.last()), std::string("2.5 s: SWR 3.4 \xc2\xb7 31% power"));
    CHECK_EQ(burstText(*m.last(), "; "), std::string("2.5 s: SWR 3.4; 31% power"));
    CHECK_EQ(m.last()->alcStepDb, 0.0);

    // The steps, from the median: to 55%, holding between 45% and 65%.
    CHECK_EQ(alcStepDb(0.9), -TxMeters::BIG_STEP_DB);
    CHECK_EQ(alcStepDb(0.7), -TxMeters::STEP_DB);
    CHECK_EQ(alcStepDb(0.65), 0.0);
    CHECK_EQ(alcStepDb(0.55), 0.0);
    CHECK_EQ(alcStepDb(0.45), 0.0);
    CHECK_EQ(alcStepDb(0.4), TxMeters::STEP_DB);
    CHECK_EQ(alcStepDb(0.1), TxMeters::BIG_UP_DB);
    auto burstOf = [&](std::initializer_list<double> alcs) {
        m.transmitting(true, 40000);
        uint64_t t = 40000;
        for (double a : alcs) m.reading(alcOnly(a), t += 250);
        m.transmitting(false, t + 100);
        return *m.last();
    };
    CHECK_EQ(burstOf({0.0, 0.9, 0.95, 0.9, 0.2}).alcStepDb, -TxMeters::BIG_STEP_DB);
    CHECK(burstOf({0.0, 0.9, 0.95, 0.9, 0.2}).overTarget());
    CHECK_EQ(burstOf({0.3, 0.35, 0.4, 0.38}).alcStepDb, TxMeters::STEP_DB);
    // One reading will do: a short burst may have no more.
    CHECK_EQ(burstOf({0.9}).alcStepDb, -TxMeters::BIG_STEP_DB);
}

void swr()
{
    // The bar, drawn on the reflection coefficient.
    CHECK_EQ(swrScale(0.0), 0.0);
    CHECK_EQ(swrScale(1.0), 0.0);
    CHECK(std::fabs(swrScale(1.5) - 0.3) < 1e-9);
    CHECK(std::fabs(swrScale(2.0) - 0.5) < 1e-9);
    CHECK(std::fabs(swrScale(3.0) - 0.75) < 1e-9);
    CHECK_EQ(swrScale(5.0), 1.0);
    CHECK_EQ(swrScale(9.0), 1.0);
    CHECK(swrBand(1.0) == SwrBand::Good && swrBand(1.49) == SwrBand::Good);
    CHECK(swrBand(1.5) == SwrBand::Fair && swrBand(1.99) == SwrBand::Fair);
    CHECK(swrBand(2.0) == SwrBand::Poor && swrBand(3.0) == SwrBand::Poor);
    CHECK(swrBand(3.01) == SwrBand::Bad);

    // The trip: off, nothing trips.
    SwrTrip trip;
    CHECK(!trip.reading(4.0, 100) && !trip.reading(4.0, 200));
    trip.setEnabled(true);
    trip.transmitting(true);
    // One noisy reading doesn't; one at 3.0 isn't over.
    CHECK(!trip.reading(3.5, 300));
    CHECK(!trip.reading(2.0, 400));
    CHECK(!trip.reading(3.5, 500));
    CHECK(!trip.reading(3.0, 600));
    // A burst's end starts the count afresh.
    CHECK(!trip.reading(3.5, 700));
    trip.transmitting(false);
    trip.transmitting(true);
    CHECK(!trip.reading(3.5, 800));
    // Two in a row do, once, and it stays tripped.
    CHECK(trip.reading(3.6, 900));
    CHECK(trip.tripped() && trip.trippedSwr() == 3.6 && trip.trippedMs() == 900);
    CHECK(!trip.reading(4.0, 1000) && !trip.reading(4.0, 1100));
    trip.transmitting(false);
    CHECK(trip.tripped());
    trip.clear();
    CHECK(!trip.tripped());
    CHECK(!trip.reading(4.0, 1200));
    CHECK(trip.reading(4.0, 1300));

    // The follower, locked out: unkeyed, and no PTT ON keys it until it is
    // cleared; PTT OFF doesn't clear it.
    PttFollower f;
    f.setEnabled(true);
    f.host(true, 0);
    CHECK(f.keyed());
    f.lockOut("the SWR went over 3:1");
    CHECK(!f.keyed() && f.lockedOut());
    f.host(false, 100);
    f.host(true, 200);
    CHECK(!f.keyed());
    CHECK_EQ(f.why(), std::string("the SWR went over 3:1"));
    f.hostLost();
    f.setEnabled(false);
    f.setEnabled(true);
    f.host(true, 300);
    CHECK(!f.keyed() && f.lockedOut());
    f.clearLockOut();
    CHECK(f.why().empty());
    // The host is still keyed: the next PTT ON keys.
    f.host(false, 400);
    f.host(true, 500);
    CHECK(f.keyed());

    // The trip box changes nothing about how the rig is opened.
    RigSettings a, b;
    a.source = b.source = RigSource::Hamlib;
    b.swrTrip = true;
    CHECK(sameRig(a, b));
}

} // namespace

TxMeters::Burst burstStep(double dB, uint64_t startMs = 100000)
{
    TxMeters::Burst burst;
    burst.startMs = startMs;
    burst.readings = 10;
    burst.alcPeak = 0.9;
    burst.alcStepDb = dB;
    return burst;
}

bool near(const std::optional<double>& value, double want)
{
    return value && std::fabs(*value - want) < 0.001;
}

// Sends what Auto ALC asks for and has the host answer it.
void apply(AutoAlc& alc, std::optional<double> next)
{
    if (!next) return;
    alc.sent(*next);
    alc.hostLevel(*next, 1);
}

// Auto ALC: to 55% both ways, around the base (the level set or found):
// at most CLIMB_DB over it, MAX_TRIM_DB under it, and the base put back
// when off.
void autoAlc()
{
    AutoAlc alc;
    // Off, it sends nothing.
    alc.hostLevel(-3.0, 0);
    CHECK(!alc.burstEnded(burstStep(-2.0)));

    CHECK(!alc.setEnabled(true));
    CHECK(near(alc.base(), -3.0));
    CHECK(near(alc.top(), -3.0 + AutoAlc::CLIMB_DB));
    CHECK(near(alc.floor(), -3.0 - AutoAlc::MAX_TRIM_DB));
    // A burst too hot: down by what the burst says, to a tenth.
    CHECK(near(alc.burstEnded(burstStep(-2.0)), -5.0));
    // The host's answer to our own TXPOWER keeps the base.
    alc.sent(-5.0);
    alc.hostLevel(-5.0, 0);
    CHECK(near(alc.base(), -3.0));
    CHECK(near(alc.level(), -5.0));
    CHECK(std::fabs(alc.offsetDb() + 2.0) < 0.01);
    // In the band: nothing.
    CHECK(!alc.burstEnded(burstStep(0.0)));
    // Too cold: up, past the base (Jeff's IC-7100 sat at 40% at the level
    // it found, 2026-10-26), to CLIMB_DB over it and no further.
    for (int i = 0; i < 20; i++) apply(alc, alc.burstEnded(burstStep(1.0)));
    CHECK(near(alc.level(), -3.0 + AutoAlc::CLIMB_DB));
    CHECK(std::fabs(alc.offsetDb() - AutoAlc::CLIMB_DB) < 0.01);
    CHECK(!alc.burstEnded(burstStep(0.5)));
    CHECK(alc.atTop());

    // No further than MAX_TRIM_DB under the base.
    for (int i = 0; i < 20; i++) apply(alc, alc.burstEnded(burstStep(-2.0)));
    CHECK(near(alc.level(), -3.0 - AutoAlc::MAX_TRIM_DB));
    CHECK(alc.atFloor() && !alc.atTop());
    CHECK(!alc.burstEnded(burstStep(-2.0)));

    // Turned off: the base goes back.
    CHECK(near(alc.setEnabled(false), -3.0));
    apply(alc, -3.0);
    CHECK(near(alc.base(), -3.0));
    CHECK(!alc.restore());

    // A level Ragtime didn't send (the operator's, in data2g-host's own
    // window) is the new base.
    alc.setEnabled(true);
    apply(alc, alc.burstEnded(burstStep(-0.5)));
    CHECK(near(alc.level(), -3.5));
    alc.hostLevel(-10.0, 200000);
    CHECK(near(alc.base(), -10.0));
    CHECK(!alc.restore());
    // A burst begun before it was sent at the old level: no step.
    CHECK(!alc.burstEnded(burstStep(-2.0, 190000)));
    CHECK(near(alc.burstEnded(burstStep(-2.0, 210000)), -12.0));
    apply(alc, -12.0);

    // So is one the operator sets in Ragtime, and a burst sent before it,
    // ending as the host answers, moves nothing: this is the -18 to -21
    // then -20.5 Jeff saw (2026-10-26).
    AutoAlc knob;
    knob.setEnabled(true);
    knob.hostLevel(-18.0, 0);
    knob.operatorSet(-21.04, 300000);
    knob.sent(-21.0);
    CHECK(near(knob.base(), -21.0));
    CHECK(!knob.burstEnded(burstStep(-2.5, 290000)));
    // A burst after it steps from the level sent, not the host's last
    // answer, even before the host has answered.
    CHECK(near(knob.burstEnded(burstStep(-0.5, 310000)), -21.5));
    knob.sent(-21.5);
    knob.hostLevel(-21.0, 311000);
    knob.hostLevel(-21.5, 311000);
    CHECK(near(knob.base(), -21.0));
    CHECK(near(knob.level(), -21.5));
    // Held while the operator turns the knob in Settings.
    knob.hold(true);
    CHECK(!knob.burstEnded(burstStep(-2.0, 320000)));
    knob.hold(false);
    // Quitting puts back the base.
    CHECK(near(knob.restore(), -21.0));

    // No host level (a host without TXPOWER, or none): nothing.
    knob.hostLevel(std::nullopt, 0);
    CHECK(!knob.burstEnded(burstStep(-2.0, 400000)));
    CHECK(!knob.restore());

    // Within TXPOWER's own range, at both ends.
    AutoAlc low;
    low.hostLevel(-59.0, 0);
    low.setEnabled(true);
    CHECK(near(low.burstEnded(burstStep(-2.0)), AutoAlc::LEVEL_MIN_DB));
    AutoAlc high;
    high.hostLevel(18.0, 0);
    high.setEnabled(true);
    for (int i = 0; i < 10; i++) apply(high, high.burstEnded(burstStep(1.0)));
    CHECK(near(high.level(), AutoAlc::LEVEL_MAX_DB));
}

int main()
{
    backendConf();
    hamlibWords();
    words();
    sameRigs();
    follows();
    safety();
    meters();
    swr();
    autoAlc();
    return ragtime_test::finish();
}
