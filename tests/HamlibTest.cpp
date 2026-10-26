// The rig library against Hamlib itself: the rig list, a backend's own
// settings, and HamlibRig opening Hamlib's dummy rig (model 1, no port, no
// radio) on its thread, reading the dial, keying and unkeying, reading
// the transmit meters while transmitting and the S-meter between bursts, and saying why when a rig can't
// be opened.
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "ragtime/rig/HamlibRig.h"

using namespace ragtime;
using rig::HamlibRig;

namespace
{

// Waits up to five seconds for the status to pass.
bool waitFor(const HamlibRig& r, const std::function<bool(const HamlibRig::Status&)>& ok)
{
    for (int i = 0; i < 500; i++)
    {
        if (ok(r.status())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

radio::RigSettings dummy()
{
    radio::RigSettings s;
    s.source = radio::RigSource::Hamlib;
    s.model = 1;
    s.keyRig = true;
    return s;
}

void list()
{
    CHECK(rig::available());
    CHECK(rig::hamlibVersion().rfind("Hamlib 4", 0) == 0);
    bool sawDummy = false, sawIc7100 = false, sawNet = false;
    for (const rig::Model& m : rig::models())
    {
        sawDummy = sawDummy || (m.id == 1 && m.maker == "Hamlib" && m.port == radio::PortType::None);
        sawIc7100 = sawIc7100 || (m.id == 3070 && m.maker == "Icom" && m.name == "IC-7100"
                                  && m.port == radio::PortType::Serial);
        sawNet = sawNet || m.id == 2;
    }
    CHECK(sawDummy && sawIc7100);
    // As a person reads them: IC-703 before IC-7000, Kachina before KTH.
    std::vector<rig::Model> all = rig::models();
    auto at = [&](int id) {
        for (size_t i = 0; i < all.size(); i++)
        {
            if (all[i].id == id) return (int)i;
        }
        return -1;
    };
    CHECK(at(3055) >= 0 && at(3060) >= 0 && at(3055) < at(3060));
    // NET rigctl is the Rig page's rigctld choice, read only.
    CHECK(!sawNet);
    CHECK(!rig::model(2));
    CHECK(rig::model(3070) && rig::model(3070)->name == "IC-7100");
    bool civ = false;
    for (const rig::BackendParam& p : rig::backendParams(3070)) civ = civ || p.name == "civaddr";
    CHECK(civ);
}

void dummyRig()
{
    HamlibRig r;
    CHECK(!r.status().running);
    r.start(dummy());
    CHECK(waitFor(r, [](const HamlibRig::Status& s) { return s.open && s.hz; }));
    HamlibRig::Status s = r.status();
    CHECK(s.running && !s.keyed && s.problem.empty());
    CHECK_EQ(s.rig, std::string("Hamlib Dummy"));
    // The dummy rig starts at 145 MHz.
    CHECK(s.hz && *s.hz == 145000000);
    // Its S-meter, between bursts, for the band's noise.
    CHECK(s.readsStrength);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.strengthReadings > 0 && st.strength; }));
    s = r.status();
    CHECK(*s.strength >= -100 && *s.strength <= 100);

    r.setPtt(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.keyed; }));
    r.setPtt(false);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.keyed; }));
    CHECK_EQ(r.status().pttFailures, (uint64_t)0);

    // The dummy rig reports ALC, SWR and power in watts. Transmitting
    // (data2g-host's PTT ON, keyed by whoever) reads them, and none
    // between bursts.
    s = r.status();
    CHECK(s.readsAlc && s.readsSwr && s.readsPower);
    CHECK(!s.meters.any());
    r.setTransmitting(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) {
        return st.meters.alc && st.meters.swr && st.meters.watts && st.meterReadings >= 3;
    }));
    CHECK(!r.status().keyed && !r.status().meters.power);
    // A PTT change still goes first.
    r.setPtt(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.keyed; }));
    r.setTransmitting(false);
    // Still keyed by us: still transmitting.
    uint64_t before = r.status().meterReadings;
    CHECK(waitFor(r, [&](const HamlibRig::Status& st) { return st.meterReadings > before; }));
    r.setPtt(false);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.keyed && !st.meters.any(); }));
    before = r.status().meterReadings;
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * HamlibRig::METER_MS));
    CHECK_EQ(r.status().meterReadings, before);

    // The SWR trip: two readings over 3:1 unkey the rig and set its power
    // to 0, and it isn't keyed again, whatever it is told, until cleared.
    r.setDummyLevelForTest("RFPOWER", 0.5);
    r.setDummyLevelForTest("SWR", 3.5);
    r.setSwrTrip(true);
    r.setTransmitting(true);
    r.setPtt(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.swrTripped; }));
    s = r.status();
    CHECK(!s.keyed && s.swrTrips == 1);
    CHECK_EQ(s.tripText, std::string("SWR 3.5: Ragtime unkeyed the rig and set its power to 0 (it was 50%)"));
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * HamlibRig::METER_MS));
    CHECK(!r.status().keyed && r.status().swrTrips == 1);
    // Cleared, with the antenna fixed: keyed again, as it is still told.
    r.setDummyLevelForTest("SWR", 1.2);
    r.clearSwrTrip();
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.swrTripped && st.keyed; }));
    r.setPtt(false);
    r.setTransmitting(false);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.keyed; }));
    r.setSwrTrip(false);

    // Keyed when stopped: unkeyed, and closed.
    r.setPtt(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.keyed; }));
    r.stop();
    CHECK(!r.status().running && !r.status().open);

    // Started again, with other settings.
    radio::RigSettings mic = dummy();
    mic.ptt = radio::PttMethod::CatMic;
    mic.pollSeconds = 2;
    r.start(mic);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.open; }));
    r.setPtt(true);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return st.keyed; }));
    r.setPtt(false);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.keyed; }));
}

void cantOpen()
{
    HamlibRig r;
    radio::RigSettings s = dummy();
    s.model = 999999;
    r.start(s);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.problem.empty(); }));
    CHECK(r.status().problem.find("no rig model 999999") != std::string::npos);
    CHECK(r.status().problem.find("trying again") != std::string::npos);
    // Keying a rig that isn't open does nothing.
    r.setPtt(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!r.status().keyed && !r.status().open);
    r.stop();

    s = dummy();
    s.backendConf = "nosuch=1";
    r.start(s);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.problem.empty(); }));
    CHECK(r.status().problem.find("no setting \"nosuch\"") != std::string::npos);
    r.stop();

    s = dummy();
    s.model = 3070;
    s.port = "/dev/ragtime-no-such-port";
    r.start(s);
    CHECK(waitFor(r, [](const HamlibRig::Status& st) { return !st.problem.empty(); }));
    CHECK(r.status().problem.find("Couldn't open the IC-7100 on /dev/ragtime-no-such-port") != std::string::npos);
    CHECK(!r.status().open);
    r.stop();
}

} // namespace

int main()
{
    list();
    dummyRig();
    cantOpen();
    return ragtime_test::finish();
}
