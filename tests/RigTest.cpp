// Rig control without a rig: the settings as Hamlib's configuration words,
// the words kept in the settings file, and the PTT follower's keying and
// its safety unkeys.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Check.h"
#include "ragtime/radio/PttFollower.h"
#include "ragtime/radio/RigSettings.h"

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

} // namespace

int main()
{
    backendConf();
    hamlibWords();
    words();
    sameRigs();
    follows();
    safety();
    return ragtime_test::finish();
}
