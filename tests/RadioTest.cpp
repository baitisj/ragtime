// The radio's side: the bands and the US data segments, the rigctld dial
// reader (it only ever asks "f", stops asking while keyed, and lets a
// reading go stale), and the path log (its lines, the file read back from
// the end, and the 24-hour strip and its words).
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/radio/Bands.h"
#include "ragtime/radio/DialReader.h"
#include "ragtime/radio/Noise.h"
#include "ragtime/radio/PathLog.h"
#include "ragtime/radio/RigFilter.h"

using namespace ragtime;
using namespace ragtime::radio;

namespace
{

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC
const uint64_t HOUR = 60ull * 60 * 1000;

void bands()
{
    CHECK_EQ(bandOf(14105000), std::string("20 m"));
    CHECK_EQ(bandOf(18100000), std::string("17 m"));
    CHECK_EQ(bandOf(5357000), std::string("60 m"));
    CHECK_EQ(bandOf(14400000), std::string(""));
    CHECK_EQ(bandOf(0), std::string(""));
    CHECK(bandOrder("160 m") < bandOrder("20 m"));
    CHECK(bandOrder("20 m") < bandOrder("2 m"));
    CHECK_EQ(bandOrder("nonsense"), amateurBands().size());

    CHECK_EQ(formatMHz(14105000), std::string("14.105 MHz"));
    CHECK_EQ(formatMHz(7074500), std::string("7.0745 MHz"));
    CHECK_EQ(formatMHz(10136123), std::string("10.136123 MHz"));
    CHECK_EQ(formatMHz(144000000), std::string("144.000 MHz"));

    // Inside, with 3 kHz to spare.
    CHECK(usDataRefusal(14105000).empty());
    CHECK(usDataRefusal(14003000).empty());
    CHECK(usDataRefusal(14147000).empty());
    CHECK(usDataRefusal(10136000).empty());
    CHECK(usDataRefusal(18105000).empty());
    CHECK(usDataRefusal(50300000).empty());
    // Too near an edge, the phone segment, outside the bands, and 60 m.
    CHECK(!usDataRefusal(14002999).empty());
    CHECK(!usDataRefusal(14148000).empty());
    std::string phone = usDataRefusal(14250000);
    CHECK(phone.find("14.250 MHz is outside the US data segments") == 0);
    CHECK(phone.find("14.000-14.150 MHz (20 m)") != std::string::npos);
    CHECK(!usDataRefusal(18120000).empty());
    CHECK(!usDataRefusal(3800000).empty());
    CHECK(!usDataRefusal(9000000).empty());
    CHECK(!usDataRefusal(0).empty());

    // 60 m: each channel only with the dial 1.5 kHz below its centre, to
    // the hertz, and the band segment with 3 kHz to spare (97.303(h)).
    for (uint64_t dial : {5330500ull, 5346500ull, 5371500ull, 5403500ull})
    {
        CHECK(usDataRefusal(dial).empty());
        CHECK(usDataPlace(dial).find("100 W ERP") != std::string::npos);
        CHECK(!usDataRefusal(dial + 1).empty());
        CHECK(!usDataRefusal(dial - 1).empty());
        CHECK(!usDataRefusal(dial + 1500).empty());
    }
    CHECK(usDataPlace(5371500).find("5.373 MHz") != std::string::npos);
    CHECK(usDataRefusal(5354500).empty());
    CHECK(usDataRefusal(5357000).empty());
    CHECK(usDataRefusal(5363500).empty());
    CHECK(usDataPlace(5360000).find("9.15 W ERP") != std::string::npos);
    CHECK(!usDataRefusal(5354499).empty());
    CHECK(!usDataRefusal(5363501).empty());
    CHECK(!usDataRefusal(5340000).empty());
    std::string sixty = usDataRefusal(5371600);
    CHECK(sixty.find("5.3716 MHz is on 60 m") == 0);
    CHECK(sixty.find("5.3305, 5.3465, 5.3715 or 5.4035 MHz") != std::string::npos);
    CHECK(sixty.find("5.3545-5.3635 MHz") != std::string::npos);
    CHECK(usDataPlace(14105000) == "Inside a US data segment: sending allowed");
    CHECK(usDataPlace(14250000).empty());
}

// Answers the meter probe rigctld gets on connecting: the levels named in
// reports answer 0, the rest RPRT -11 (not available). Returns what was
// asked after the probe.
std::string probe(DialReader& dial, uint64_t t, const std::string& reports = "")
{
    std::string out = dial.takeOutput();
    while (out.compare(0, 3, "+l ") == 0)
    {
        std::string level = out.substr(out.rfind(' ') + 1);
        level.pop_back();
        bool known = ("," + reports + ",").find("," + level + ",") != std::string::npos;
        std::string answer = "get_level: " + level + "\n" + (known ? "0.000000\nRPRT 0\n" : "RPRT -11\n");
        dial.bytes(answer.data(), answer.size(), t);
        out = dial.takeOutput();
    }
    return out;
}

void say(DialReader& dial, const std::string& text, uint64_t t)
{
    dial.bytes(text.data(), text.size(), t);
}

void dialReader()
{
    DialReader dial;
    uint64_t t = T0;
    CHECK(!dial.dialHz(t));
    dial.tick(t);
    CHECK_EQ(dial.takeOutput(), std::string(""));

    dial.connected(t);
    // First, whether rigctld was started with --vfo; this one wasn't.
    CHECK_EQ(dial.takeOutput(), std::string("\\chk_vfo\n"));
    dial.bytes("0\n", 2, t);
    CHECK(!dial.vfoMode());
    // Then which meters it reports (none, here), then the dial.
    CHECK_EQ(probe(dial, t), std::string("f\n"));
    CHECK(!dial.readsAny());
    // Not again until answered or ANSWER_MS.
    dial.tick(t + 500);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    dial.bytes("14105000\n", 9, t + 100);
    CHECK(dial.dialHz(t + 100) == std::optional<uint64_t>(14105000));
    // Once a second after that.
    dial.tick(t + 900);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    dial.tick(t + 1000);
    CHECK_EQ(dial.takeOutput(), std::string("f\n"));
    // An answer in two pieces, with CR LF and a decimal part.
    dial.bytes("7074", 4, t + 1100);
    dial.bytes("500.000000\r\n", 12, t + 1100);
    CHECK(dial.dialHz(t + 1100) == std::optional<uint64_t>(7074500));

    // An error leaves the reading as it was, and says so.
    dial.tick(t + 2100);
    CHECK_EQ(dial.takeOutput(), std::string("f\n"));
    dial.bytes("RPRT -11\n", 9, t + 2200);
    CHECK_EQ(dial.problem(), std::string("rigctld answered RPRT -11"));
    CHECK(dial.dialHz(t + 2200) == std::optional<uint64_t>(7074500));

    // No answer: asked again after ANSWER_MS, and the reading goes stale.
    dial.tick(t + 3200);
    std::string asked = dial.takeOutput();
    CHECK_EQ(asked, std::string("f\n"));
    dial.tick(t + 1100 + DialReader::STALE_MS + 1);
    CHECK(!dial.dialHz(t + 1100 + DialReader::STALE_MS + 1));
    CHECK(!dial.problem().empty());

    // Keyed: no asking (no meters here), and the reading stands however
    // long the burst.
    t += 10000;
    dial.bytes("14105000\n", 9, t);
    CHECK(dial.problem().empty());
    dial.takeOutput();
    dial.setKeyed(true, t + 100);
    for (uint64_t ms = t + 200; ms < t + 30000; ms += 100) dial.tick(ms);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    CHECK(dial.dialHz(t + 30000) == std::optional<uint64_t>(14105000));
    // Unkeyed: asked at once, and the reading stands a moment longer.
    dial.setKeyed(false, t + 30000);
    CHECK_EQ(dial.takeOutput(), std::string("f\n"));
    CHECK(dial.dialHz(t + 31000) == std::optional<uint64_t>(14105000));

    // Lines that aren't frequencies are ignored, and so is anything under
    // 1 kHz (a late "1" from \chk_vfo).
    dial.bytes("garbage\n1e9\n.\n0\n1\n", 19, t + 31100);
    CHECK(dial.dialHz(t + 31100) == std::optional<uint64_t>(14105000));

    dial.disconnected();
    CHECK(!dial.dialHz(t + 31200));
    dial.tick(t + 40000);
    CHECK_EQ(dial.takeOutput(), std::string(""));
}

// A rigctld started with --vfo wants the VFO named: "f currVFO".
void vfoMode()
{
    uint64_t t = T0;
    auto asked = [&](const char* answer) {
        DialReader dial;
        dial.connected(t);
        CHECK_EQ(dial.takeOutput(), std::string("\\chk_vfo\n"));
        std::string line = std::string(answer) + "\n";
        dial.bytes(line.data(), line.size(), t + 50);
        return probe(dial, t + 50);
    };
    CHECK_EQ(asked("1"), std::string("f currVFO\n"));
    CHECK_EQ(asked("CHKVFO 1"), std::string("f currVFO\n"));
    CHECK_EQ(asked("0"), std::string("f\n"));
    CHECK_EQ(asked("CHKVFO 0"), std::string("f\n"));
    // A rigctld that doesn't know \chk_vfo.
    CHECK_EQ(asked("RPRT -4"), std::string("f\n"));

    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    dial.bytes("1\n", 2, t + 50);
    CHECK(dial.vfoMode());
    // The meters name the VFO too.
    CHECK_EQ(dial.takeOutput(), std::string("+l currVFO ALC\n"));
    say(dial, "get_level: currVFO ALC\n0.000000\nRPRT 0\n", t + 60);
    CHECK(dial.reads(DialReader::Meter::Alc));
    CHECK_EQ(probe(dial, t + 60), std::string("f currVFO\n"));
    dial.bytes("14105000\n", 9, t + 100);
    CHECK(dial.dialHz(t + 100) == std::optional<uint64_t>(14105000));
    dial.tick(t + 1100);
    CHECK_EQ(dial.takeOutput(), std::string("f currVFO\n"));
    // Asked afresh on the next connection.
    dial.disconnected();
    dial.connected(t + 2000);
    CHECK(!dial.vfoMode());
    CHECK(!dial.readsAny());
    CHECK_EQ(dial.takeOutput(), std::string("\\chk_vfo\n"));

    // No answer to \chk_vfo at all: no VFO names after ANSWER_MS, and
    // nothing before.
    DialReader quiet;
    quiet.connected(t);
    quiet.takeOutput();
    quiet.tick(t + DialReader::ANSWER_MS - 1);
    CHECK_EQ(quiet.takeOutput(), std::string(""));
    quiet.tick(t + DialReader::ANSWER_MS);
    CHECK_EQ(probe(quiet, t + DialReader::ANSWER_MS), std::string("f\n"));

    // Keyed while it asks: the meters, never the dial.
    DialReader keyed;
    keyed.connected(t);
    keyed.setKeyed(true, t + 10);
    keyed.bytes("0\n", 2, t + 20);
    CHECK_EQ(keyed.takeOutput(), std::string("\\chk_vfo\n+l ALC\n"));
}

// The transmit meters while keyed.
void meters()
{
    using Meter = DialReader::Meter;
    uint64_t t = T0;
    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    dial.bytes("0\n", 2, t);
    CHECK_EQ(probe(dial, t, "ALC,SWR,RFPOWER_METER_WATTS"), std::string("f\n"));
    CHECK(dial.reads(Meter::Alc));
    CHECK(dial.reads(Meter::Swr));
    CHECK(dial.reads(Meter::Watts));
    CHECK(!dial.reads(Meter::Power));
    // The probe's readings (not keyed) aren't kept.
    CHECK(dial.takeReadings().empty());
    say(dial, "7065000\n", t + 10);

    // Keyed: each meter in turn, METER_MS after the last answer.
    t += 2000;
    dial.setKeyed(true, t);
    CHECK_EQ(dial.takeOutput(), std::string("+l ALC\n"));
    say(dial, "get_level: ALC\n0.420000\nRPRT 0\n", t + 30);
    dial.tick(t + 30 + DialReader::METER_MS - 1);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    dial.tick(t + 30 + DialReader::METER_MS);
    CHECK_EQ(dial.takeOutput(), std::string("+l SWR\n"));
    say(dial, "get_level: SWR\n1.300000\nRPRT 0\n", t + 300);
    dial.tick(t + 600);
    CHECK_EQ(dial.takeOutput(), std::string("+l RFPOWER_METER_WATTS\n"));
    say(dial, "get_level: RFPOWER_METER_WATTS\n38.000000\nRPRT 0\n", t + 650);
    std::vector<MeterReading> got = dial.takeReadings();
    CHECK_EQ(got.size(), (size_t)3);
    CHECK(got[0].alc && *got[0].alc > 0.41 && *got[0].alc < 0.43 && !got[0].swr);
    CHECK(got[1].swr && *got[1].swr > 1.29 && *got[1].swr < 1.31);
    CHECK(got[2].watts && *got[2].watts > 37.9 && !got[2].power);
    // The dial stands through the burst.
    CHECK(dial.dialHz(t + 20000) == std::optional<uint64_t>(7065000));

    // A long answer that comes late is still read for the meter it names,
    // and the question out waits for its own.
    dial.tick(t + 1000);
    CHECK_EQ(dial.takeOutput(), std::string("+l ALC\n"));
    say(dial, "get_level: SWR\n5.000000\nRPRT 0\n", t + 1010);
    got = dial.takeReadings();
    CHECK_EQ(got.size(), (size_t)1);
    CHECK(got[0].swr && *got[0].swr > 4.9);
    dial.tick(t + 1500);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    say(dial, "get_level: ALC\n0.100000\nRPRT 0\n", t + 1520);

    // With the SWR trip on, every other question is the SWR.
    dial.setSwrWatch(true);
    std::string swrs;
    uint64_t at = t + 2000;
    for (int i = 0; i < 8; i++, at += 300)
    {
        dial.tick(at);
        std::string q = dial.takeOutput();
        std::string level = q.substr(3, q.size() - 4);
        swrs += level == "SWR" ? "S" : "o";
        say(dial, "get_level: " + level + "\n1.0\nRPRT 0\n", at + 10);
    }
    CHECK_EQ(swrs, std::string("SoSoSoSo"));
    dial.setSwrWatch(false);
    dial.takeReadings();

    // A meter that fails twice in a row while keyed is asked no more.
    for (int i = 0; i < 12; i++, at += 300)
    {
        dial.tick(at);
        std::string q = dial.takeOutput();
        if (q.empty()) continue;
        std::string level = q.substr(3, q.size() - 4);
        say(dial, "get_level: " + level + "\n" + (level == "SWR" ? "RPRT -9\n" : "0.5\nRPRT 0\n"), at + 10);
    }
    CHECK(!dial.reads(Meter::Swr));
    for (int i = 0; i < 12; i++, at += 300)
    {
        dial.tick(at);
        std::string q = dial.takeOutput();
        CHECK(q.find("SWR") == std::string::npos);
        if (!q.empty()) say(dial, "get_level: " + q.substr(3, q.size() - 4) + "\n0.5\nRPRT 0\n", at + 10);
    }

    // No answer at all: given up after ANSWER_MS, and the next asked.
    dial.tick(at);
    std::string q = dial.takeOutput();
    CHECK(!q.empty());
    dial.tick(at + DialReader::ANSWER_MS - 1);
    CHECK_EQ(dial.takeOutput(), std::string(""));
    dial.tick(at + DialReader::ANSWER_MS);
    CHECK(!dial.takeOutput().empty());
    say(dial, "get_level: ALC\n0.5\nRPRT 0\n", at + DialReader::ANSWER_MS + 10);
    // Late answers to the one given up are read, and end nothing.
    say(dial, "get_level: RFPOWER_METER_WATTS\n0.5\nRPRT 0\n", at + DialReader::ANSWER_MS + 20);

    // Unkeyed: the dial again, at once.
    at += 10000;
    dial.tick(at);
    dial.takeOutput();
    say(dial, "get_level: ALC\n0.5\nRPRT 0\nget_level: RFPOWER_METER_WATTS\n9\nRPRT 0\n", at);
    dial.setKeyed(false, at + 100);
    CHECK_EQ(dial.takeOutput(), std::string("f\n"));

    // A rig that knows no watts: a share of full power instead.
    DialReader share;
    share.connected(t);
    share.takeOutput();
    share.bytes("0\n", 2, t);
    CHECK_EQ(probe(share, t, "ALC,RFPOWER_METER"), std::string("f\n"));
    CHECK(share.reads(Meter::Power));
    CHECK(!share.reads(Meter::Swr));
    CHECK(share.readsPower());
    // A probe error that may only mean "not transmitting" is asked again
    // when keyed.
    DialReader later;
    later.connected(t);
    later.takeOutput();
    later.bytes("0\n", 2, t);
    CHECK_EQ(later.takeOutput(), std::string("+l ALC\n"));
    say(later, "get_level: ALC\nRPRT -9\n", t + 10);
    CHECK_EQ(probe(later, t + 10), std::string("f\n"));
    say(later, "7065000\n", t + 20);
    CHECK(!later.reads(Meter::Alc));
    later.setKeyed(true, t + 2000);
    CHECK_EQ(later.takeOutput(), std::string("+l ALC\n"));
    say(later, "get_level: ALC\n0.3\nRPRT 0\n", t + 2010);
    CHECK(later.reads(Meter::Alc));
}

// The S-meter, between bursts, every STRENGTH_MS, and not straight after
// one; and the noise floor made of it.
void strength()
{
    using Meter = DialReader::Meter;
    uint64_t t = T0;
    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    dial.bytes("0\n", 2, t);
    CHECK_EQ(probe(dial, t, "SWR,STRENGTH"), std::string("f\n"));
    CHECK(dial.readsStrength());
    CHECK(dial.reads(Meter::Swr));
    // The probe's 0 is a reading too.
    CHECK_EQ(dial.takeStrength().size(), (size_t)1);
    say(dial, "7065000\n", t + 10);
    // The probe asked it just now, so not again until STRENGTH_MS.
    dial.tick(t + 500);
    CHECK_EQ(dial.takeOutput(), std::string(""));

    // Between dial readings once STRENGTH_MS is up.
    std::string all;
    for (uint64_t ms = t + 100; ms <= t + DialReader::STRENGTH_MS + 900; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "f\n") say(dial, "7065000\n", ms);
        if (out == "+l STRENGTH\n") say(dial, "get_level: STRENGTH\n-40\nRPRT 0\n", ms);
        all += out;
    }
    CHECK(all.find("+l STRENGTH\n") != std::string::npos);
    CHECK_EQ(all.find("+l STRENGTH\n"), all.rfind("+l STRENGTH\n"));
    std::vector<double> got = dial.takeStrength();
    CHECK_EQ(got.size(), (size_t)1);
    CHECK(got[0] == -40.0);

    // Keyed: never the S-meter (it would read our own signal), and an
    // answer that comes then isn't kept.
    t += 20000;
    dial.setKeyed(true, t);
    all.clear();
    for (uint64_t ms = t; ms < t + 15000; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "+l SWR\n") say(dial, "get_level: SWR\n1.2\nRPRT 0\n", ms);
        all += out;
    }
    CHECK(all.find("STRENGTH") == std::string::npos);
    say(dial, "get_level: STRENGTH\n10\nRPRT 0\n", t + 15000);
    CHECK(dial.takeStrength().empty());

    // Unkeyed: not in the first SETTLE_MS, while the receiver recovers.
    t += 15000;
    dial.setKeyed(false, t);
    all.clear();
    uint64_t first = 0;
    for (uint64_t ms = t; ms < t + 4000; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "f\n") say(dial, "7065000\n", ms);
        if (out == "+l STRENGTH\n")
        {
            if (!first) first = ms;
            say(dial, "get_level: STRENGTH\n-38\nRPRT 0\n", ms);
        }
    }
    CHECK(first >= t + DialReader::SETTLE_MS);
    CHECK(first < t + DialReader::SETTLE_MS + 1100);

    // A rig whose S-meter fails twice between bursts: dropped.
    DialReader failing;
    failing.connected(T0);
    failing.takeOutput();
    failing.bytes("0\n", 2, T0);
    probe(failing, T0, "STRENGTH");
    for (uint64_t ms = T0; ms < T0 + 30000; ms += 100)
    {
        failing.tick(ms);
        std::string out = failing.takeOutput();
        if (out == "f\n") say(failing, "7065000\n", ms);
        if (out == "+l STRENGTH\n") say(failing, "get_level: STRENGTH\nRPRT -5\n", ms);
    }
    CHECK(!failing.readsStrength());

    // S-units: S9 is 0 dB, 6 dB each.
    CHECK_EQ(sUnitText(-36), std::string("S3"));
    CHECK_EQ(sUnitText(-40), std::string("S2"));
    CHECK_EQ(sUnitText(0), std::string("S9"));
    CHECK_EQ(sUnitText(11), std::string("S9+10"));
    CHECK_EQ(sUnitText(-80), std::string("S0"));
    CHECK(sUnits(-36) == 3.0);
    CHECK_EQ(levelText(-40), std::string("-40"));
    CHECK_EQ(levelText(-39.6), std::string("-40"));
    CHECK_EQ(levelText(12), std::string("+12"));
    CHECK_EQ(levelText(0), std::string("0"));

    // The floor: the quietest reading of the last minute, gone when stale.
    NoiseFloor floor;
    CHECK(!floor.dbOverS9(T0));
    floor.reading(-30, T0);
    floor.reading(-42, T0 + 5000);
    floor.reading(-20, T0 + 10000);     // a station's burst
    CHECK(floor.dbOverS9(T0 + 10000) == std::optional<double>(-42));
    floor.reading(-35, T0 + 64000);
    floor.reading(-36, T0 + 66000);
    CHECK(floor.dbOverS9(T0 + 66000) == std::optional<double>(-36));
    CHECK(!floor.dbOverS9(T0 + 66000 + NoiseFloor::STALE_MS + 1));
    floor.clear();
    CHECK(!floor.dbOverS9(T0 + 66000));
}

// While the Ear is open: the mode and its width, and the PBT, between
// bursts, in the long answers and the short.
void filter()
{
    uint64_t t = T0;
    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    say(dial, "0\n", t);
    CHECK_EQ(probe(dial, t), std::string("f\n"));
    say(dial, "14105000\n", t);
    // Not asked until the Ear opens.
    std::string all;
    for (uint64_t ms = t; ms < t + 5000; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "f\n") say(dial, "14105000\n", ms);
        all += out;
    }
    CHECK(all.find("+m") == std::string::npos);
    CHECK(all.find("PBT") == std::string::npos);
    CHECK(!dial.filter());

    t += 5000;
    dial.setFilterWatch(true, t);
    all.clear();
    int modes = 0;
    for (uint64_t ms = t; ms < t + 5000; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "f\n") say(dial, "14105000\n", ms);
        if (out == "+m\n")
        {
            modes++;
            say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", ms);
        }
        if (out == "+l PBT_IN\n") say(dial, "get_level: PBT_IN\n0.501961\nRPRT 0\n", ms);
        if (out == "+l PBT_OUT\n") say(dial, "get_level: PBT_OUT\n0.623529\nRPRT 0\n", ms);
        if (out == "+u MN\n") say(dial, "get_func: MN\n1\nRPRT 0\n", ms);
        if (out == "+l NOTCHF_RAW\n") say(dial, "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n", ms);
        all += out;
    }
    // Every FILTER_MS, with the dial still once a second.
    CHECK(modes >= 2 && modes <= 3);
    CHECK(all.find("+l PBT_IN\n") != std::string::npos);
    CHECK(dial.dialHz(t + 5000) == std::optional<uint64_t>(14105000));
    CHECK(dial.filter().has_value());
    if (dial.filter())
    {
        CHECK_EQ(dial.filter()->mode, std::string("USB"));
        CHECK_EQ(dial.filter()->widthHz, 2400);
        CHECK(dial.filter()->pbtIn && pbtCentred(*dial.filter()->pbtIn));
        CHECK(dial.filter()->pbtOut && !pbtCentred(*dial.filter()->pbtOut));
        CHECK(dial.filter()->notchOn && *dial.filter()->notchOn);
        CHECK(dial.filter()->notchRaw && std::fabs(*dial.filter()->notchRaw - 0.682353) < 1e-6);
        CHECK_EQ(describe(*dial.filter()),
                 std::string("USB \xc2\xb7 2400 Hz filter \xc2\xb7 PBT in 0.50, out 0.62 \xc2\xb7 notch 0.68"));
    }

    // Keyed: none of it (but a question asked before).
    t += 5000;
    dial.takeOutput();
    dial.setKeyed(true, t);
    all.clear();
    for (uint64_t ms = t; ms < t + 6000; ms += 100)
    {
        dial.tick(ms);
        all += dial.takeOutput();
    }
    CHECK(all.find("+m") == std::string::npos);
    CHECK(all.find("PBT") == std::string::npos);
    dial.setKeyed(false, t + 6000);

    // The Ear closed: not asked, and forgotten.
    t += 10000;
    dial.setFilterWatch(false, t);
    CHECK(!dial.filter());
    all.clear();
    for (uint64_t ms = t; ms < t + 6000; ms += 100)
    {
        dial.tick(ms);
        std::string out = dial.takeOutput();
        if (out == "f\n") say(dial, "14105000\n", ms);
        all += out;
    }
    CHECK(all.find("+m") == std::string::npos);

    // A rigctld started with --vfo, giving short answers; a rig with no PBT.
    DialReader vfo;
    vfo.connected(T0);
    vfo.takeOutput();
    say(vfo, "1\n", T0);
    CHECK_EQ(probe(vfo, T0), std::string("f currVFO\n"));
    say(vfo, "7074000\n", T0);
    vfo.setFilterWatch(true, T0);
    CHECK_EQ(vfo.takeOutput(), std::string("+m currVFO\n"));
    say(vfo, "PKTUSB\n3000\n", T0 + 10);
    CHECK_EQ(vfo.takeOutput(), std::string("+l currVFO PBT_IN\n"));
    say(vfo, "get_level: currVFO PBT_IN\nRPRT -11\n", T0 + 20);
    CHECK_EQ(vfo.takeOutput(), std::string("+l currVFO PBT_OUT\n"));
    say(vfo, "get_level: currVFO PBT_OUT\nRPRT -11\n", T0 + 30);
    // No manual notch: its position isn't asked.
    CHECK_EQ(vfo.takeOutput(), std::string("+u currVFO MN\n"));
    say(vfo, "get_func: currVFO MN\nRPRT -11\n", T0 + 40);
    CHECK(vfo.filter().has_value());
    if (vfo.filter())
    {
        CHECK_EQ(describe(*vfo.filter()), std::string("PKTUSB \xc2\xb7 3000 Hz filter"));
        CHECK(!vfo.filter()->pbtIn);
    }
    // PBT not asked again; the mode is.
    all.clear();
    for (uint64_t ms = T0 + 100; ms < T0 + 5000; ms += 100)
    {
        vfo.tick(ms);
        std::string out = vfo.takeOutput();
        if (out == "f currVFO\n") say(vfo, "7074000\n", ms);
        if (out == "+m currVFO\n") say(vfo, "get_mode: currVFO\nMode: PKTUSB\nPassband: 2400\nRPRT 0\n", ms);
        all += out;
    }
    CHECK(all.find("PBT") == std::string::npos);
    CHECK(all.find("MN") == std::string::npos);
    CHECK(all.find("NOTCHF") == std::string::npos);
    CHECK(all.find("+m currVFO\n") != std::string::npos);
    CHECK(vfo.filter() && vfo.filter()->widthHz == 2400);

    // A rig that can't say its mode: dropped, the dial unharmed.
    DialReader mute;
    mute.connected(T0);
    mute.takeOutput();
    say(mute, "0\n", T0);
    probe(mute, T0);
    say(mute, "7074000\n", T0);
    mute.setFilterWatch(true, T0);
    CHECK_EQ(mute.takeOutput(), std::string("+m\n"));
    say(mute, "get_mode:\nRPRT -11\n", T0 + 10);
    CHECK(mute.modeDropped());
    CHECK(!mute.filter());

    // The words.
    CHECK(pbtCentred(128.0 / 255.0));
    CHECK(pbtCentred(0.5));
    CHECK(!pbtCentred(0.52));
    CHECK(pbtCentred(0.5 + 1.0 / 255.0));
    CHECK(!pbtCentred(0.0));
    CHECK(!pbtCentred(0.4));
    CHECK_EQ(pbtText(120), std::string("+120 Hz"));
    CHECK_EQ(pbtText(-1280), std::string("-1280 Hz"));
    RigFilter f;
    f.mode = "USB";
    f.widthHz = 2400;
    f.pbtIn = 0.5;
    f.pbtOut = 0.5;
    CHECK_EQ(describe(f), std::string("USB \xc2\xb7 2400 Hz filter \xc2\xb7 PBT centred"));
}

// The Ear setting the filter (its box ticked: the caller's check): the
// knobs alone when the width stands, the mode line first when it changes,
// never while keyed, and a refusal stops the rest.
void filterSet()
{
    uint64_t t = T0;
    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    say(dial, "0\n", t);
    CHECK_EQ(probe(dial, t), std::string("f\n"));
    say(dial, "14105000\n", t);
    dial.setFilterWatch(true, t);
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+l PBT_IN\n"));
    say(dial, "get_level: PBT_IN\n0.501961\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+l PBT_OUT\n"));
    say(dial, "get_level: PBT_OUT\n0.501961\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+u MN\n"));
    say(dial, "get_func: MN\n0\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+l NOTCHF_RAW\n"));
    say(dial, "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n", t);
    CHECK(dial.filter().has_value());
    CHECK_EQ(dial.takeOutput(), std::string(""));

    RigFilter f;
    f.mode = "USB";
    f.widthHz = 2400;
    f.pbtIn = 0.65098;
    f.pbtOut = 0.5;
    f.notchOn = false;
    f.notchRaw = 0.682353;
    t += 100;
    dial.setFilter(f, t);
    // Only what differs from the reading: PBT_OUT at 0.501961 is the rig's
    // step for 0.5.
    CHECK_EQ(dial.takeOutput(), std::string("L PBT_IN 0.6510\n"));
    say(dial, "RPRT 0\n", t);
    // Read back at once.
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+l PBT_IN\n"));
    say(dial, "get_level: PBT_IN\n0.650980\nRPRT 0\n", t);
    // Whole readings only: not the new in knob with the old out one.
    CHECK(dial.filter() && dial.filter()->pbtIn && *dial.filter()->pbtIn < 0.6);
    CHECK_EQ(dial.takeOutput(), std::string("+l PBT_OUT\n"));
    say(dial, "get_level: PBT_OUT\n0.501961\nRPRT 0\n", t);
    CHECK(dial.filter() && dial.filter()->pbtIn && *dial.filter()->pbtIn < 0.6);
    CHECK_EQ(dial.takeOutput(), std::string("+u MN\n"));
    say(dial, "get_func: MN\n0\nRPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+l NOTCHF_RAW\n"));
    say(dial, "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n", t);
    CHECK(dial.filter() && dial.filter()->pbtIn && std::fabs(*dial.filter()->pbtIn - 0.65098) < 1e-4);

    // The notch: moved, then on; then off before it moves back.
    t += 100;
    f.notchOn = true;
    f.notchRaw = 0.25;
    dial.setFilter(f, t);
    // No knob turned for the notch alone.
    CHECK_EQ(dial.takeOutput(), std::string("L NOTCHF_RAW 0.2500\n"));
    say(dial, "RPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("U MN 1\n"));
    say(dial, "RPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    say(dial, dial.takeOutput() == "+l PBT_IN\n" ? "get_level: PBT_IN\n0.650980\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l PBT_OUT\n" ? "get_level: PBT_OUT\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+u MN\n" ? "get_func: MN\n1\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l NOTCHF_RAW\n" ? "get_level: NOTCHF_RAW\n0.250980\nRPRT 0\n" : "", t);
    CHECK(dial.filter() && dial.filter()->notchOn == std::optional<bool>(true));
    t += 100;
    f.notchOn = false;
    f.notchRaw = 0.682353;
    dial.setFilter(f, t);
    CHECK_EQ(dial.takeOutput(), std::string("U MN 0\n"));
    say(dial, "RPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("L NOTCHF_RAW 0.6824\n"));
    say(dial, "RPRT 0\n", t);
    dial.takeOutput();
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    say(dial, dial.takeOutput() == "+l PBT_IN\n" ? "get_level: PBT_IN\n0.650980\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l PBT_OUT\n" ? "get_level: PBT_OUT\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+u MN\n" ? "get_func: MN\n0\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l NOTCHF_RAW\n" ? "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n" : "", t);
    CHECK(dial.filter() && dial.filter()->notchOn == std::optional<bool>(false));
    CHECK_EQ(dial.filterWrites(), (uint64_t)3);
    CHECK(dial.filterProblem().empty());

    // Keyed: held until the burst ends, then the width first.
    t += 100;
    dial.setKeyed(true, t);
    dial.takeOutput();
    f.widthHz = 1800;
    dial.setFilter(f, t);
    std::string all;
    for (uint64_t ms = t; ms < t + 6000; ms += 100)
    {
        dial.tick(ms);
        all += dial.takeOutput();
    }
    CHECK(all.find("M ") == std::string::npos);
    CHECK(all.find("L ") == std::string::npos);
    t += 6000;
    dial.setKeyed(false, t);
    CHECK_EQ(dial.takeOutput(), std::string("M USB 1800\n"));
    // Refused: the knobs aren't sent, and the rig is read again.
    say(dial, "RPRT -11\n", t);
    CHECK_EQ(dial.filterProblem(), std::string("rigctld refused M USB 1800: RPRT -11"));
    all = dial.takeOutput();
    CHECK(all.find("L ") == std::string::npos);
    CHECK_EQ(dial.filterWrites(), (uint64_t)4);

    // A --vfo rigctld names the VFO.
    DialReader vfo;
    vfo.connected(T0);
    vfo.takeOutput();
    say(vfo, "1\n", T0);
    CHECK_EQ(probe(vfo, T0), std::string("f currVFO\n"));
    say(vfo, "7074000\n", T0);
    f.mode = "PKTUSB";
    f.pbtOut.reset();
    f.notchOn.reset();
    f.notchRaw.reset();
    vfo.setFilter(f, T0);
    CHECK_EQ(vfo.takeOutput(), std::string("M currVFO PKTUSB 1800\n"));
    say(vfo, "RPRT 0\n", T0);
    CHECK_EQ(vfo.takeOutput(), std::string("L currVFO PBT_IN 0.6510\n"));
    say(vfo, "RPRT 0\n", T0);
    CHECK(vfo.filterProblem().empty());

    // Quitting: the put back goes at once, every line, keyed or not.
    vfo.takeOutput();
    vfo.setKeyed(true, T0 + 10);
    vfo.takeOutput();
    f.widthHz = 3000;
    f.pbtOut = 0.5;
    vfo.setFilter(f, T0 + 20);
    CHECK_EQ(vfo.takeOutput(), std::string(""));
    vfo.flushFilter();
    // PBT_IN went already, though not yet read back.
    CHECK_EQ(vfo.takeOutput(), std::string("M currVFO PKTUSB 3000\nL currVFO PBT_OUT 0.5000\n"));
}

// A second write before the first is read back (an Undo at once): each
// part goes against what was written, not the reading from before it.
void filterSetTwice()
{
    uint64_t t = T0;
    DialReader dial;
    dial.connected(t);
    dial.takeOutput();
    say(dial, "0\n", t);
    CHECK_EQ(probe(dial, t), std::string("f\n"));
    say(dial, "14105000\n", t);
    dial.setFilterWatch(true, t);
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    say(dial, dial.takeOutput() == "+l PBT_IN\n" ? "get_level: PBT_IN\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l PBT_OUT\n" ? "get_level: PBT_OUT\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+u MN\n" ? "get_func: MN\n0\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l NOTCHF_RAW\n" ? "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n" : "", t);
    CHECK(dial.filter().has_value());
    CHECK_EQ(dial.takeOutput(), std::string(""));

    RigFilter was = *dial.filter();
    RigFilter f = was;
    f.pbtIn = 0.65098;
    t += 100;
    dial.setFilter(f, t);
    CHECK_EQ(dial.takeOutput(), std::string("L PBT_IN 0.6510\n"));
    say(dial, "RPRT 0\n", t);
    // Read back at once; Undo before the reading is whole.
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    dial.setFilter(was, t);
    CHECK_EQ(dial.takeOutput(), std::string("+l PBT_IN\n"));
    say(dial, "get_level: PBT_IN\n0.650980\nRPRT 0\n", t);
    // The stale reading said 0.50: the knob still goes back.
    CHECK_EQ(dial.takeOutput(), std::string("L PBT_IN 0.5020\n"));
    say(dial, "RPRT 0\n", t);
    CHECK_EQ(dial.takeOutput(), std::string("+m\n"));
    say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
    say(dial, dial.takeOutput() == "+l PBT_IN\n" ? "get_level: PBT_IN\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l PBT_OUT\n" ? "get_level: PBT_OUT\n0.501961\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+u MN\n" ? "get_func: MN\n0\nRPRT 0\n" : "", t);
    say(dial, dial.takeOutput() == "+l NOTCHF_RAW\n" ? "get_level: NOTCHF_RAW\n0.682353\nRPRT 0\n" : "", t);
    CHECK(dial.filter() && *dial.filter() == was);
    // Read back whole: the rig's word again, so the same write sends nothing.
    t += 100;
    dial.setFilter(was, t);
    CHECK(dial.takeOutput().find("L ") == std::string::npos);
    CHECK_EQ(dial.filterWrites(), (uint64_t)2);
}

// Every line Ragtime could ever write to rigctld is a question: whether
// rigctld wants VFOs named, the frequency, and the meters. (The Ear's
// filter settings, above, go only when it asks.)
void onlyAsks()
{
    const std::vector<std::string> allowed = {"\\chk_vfo", "f", "+l ALC", "+l SWR", "+l RFPOWER_METER_WATTS",
                                              "+l RFPOWER_METER", "+l STRENGTH", "+m", "+l PBT_IN", "+l PBT_OUT",
                                              "+u MN", "+l NOTCHF_RAW"};
    DialReader dial;
    std::string all;
    uint64_t t = T0;
    dial.connected(t);
    for (int i = 0; i < 400; i++)
    {
        t += 250;
        dial.tick(t);
        if (i % 7 == 0) dial.bytes("RPRT -1\n", 8, t);
        if (i % 5 == 0) dial.bytes("14070000\n", 9, t);
        if (i % 3 == 0) say(dial, "get_level: SWR\n1.2\nRPRT 0\n", t);
        if (i % 11 == 0) say(dial, "get_level: ALC\nRPRT -9\n", t);
        if (i % 40 == 0) dial.setKeyed(i % 80 == 0, t);
        dial.setSwrWatch(i % 120 < 60);
        dial.setFilterWatch(i % 90 < 45, t);
        if (i % 13 == 0) say(dial, "get_mode:\nMode: USB\nPassband: 2400\nRPRT 0\n", t);
        all += dial.takeOutput();
    }
    CHECK(!all.empty());
    size_t from = 0;
    for (size_t eol; (eol = all.find('\n', from)) != std::string::npos; from = eol + 1)
    {
        std::string line = all.substr(from, eol - from);
        bool ok = false;
        for (const std::string& a : allowed) ok = ok || line == a;
        CHECK(ok);
    }
    CHECK_EQ(from, all.size());
}

PathEntry entry(uint64_t at, const std::string& call, std::optional<double> snr, const std::string& band)
{
    PathEntry e;
    e.atMs = at;
    e.call = call;
    e.snrDb = snr;
    e.band = band;
    return e;
}

void lines()
{
    uint64_t ms = 0;
    CHECK_EQ(utcStamp(T0 + 16 * HOUR + 30 * 60000 + 12000), std::string("2026-10-22T16:30:12Z"));
    CHECK(parseUtcStamp("2026-10-22T16:30:12Z", ms));
    CHECK_EQ(ms, T0 + 16 * HOUR + 30 * 60000 + 12000);
    CHECK(parseUtcStamp("2024-02-29T23:59:59Z", ms));
    CHECK_EQ(utcStamp(ms), std::string("2024-02-29T23:59:59Z"));
    CHECK(!parseUtcStamp("2026-10-22 16:30:12", ms));
    CHECK(!parseUtcStamp("2026-13-22T16:30:12Z", ms));

    PathEntry e = entry(T0 + 1234, "KC2G", -6.5, "17 m");
    e.mode = "qpsk-r1/3";
    e.dialHz = 18104000;
    e.gear = "fsk32r62-r1/3";
    std::string line = pathLine(e);
    CHECK_EQ(line, std::string("2026-10-22T00:00:01Z\tKC2G\t-6.5\tqpsk-r1/3\t17 m\t18104000\tfsk32r62-r1/3\tD2G"));
    PathEntry back;
    CHECK(parsePathLine(line, back));
    CHECK_EQ(back.atMs, T0 + 1000);
    CHECK_EQ(back.call, std::string("KC2G"));
    CHECK(back.snrDb && *back.snrDb == -6.5);
    CHECK_EQ(back.mode, std::string("qpsk-r1/3"));
    CHECK_EQ(back.band, std::string("17 m"));
    CHECK_EQ(back.dialHz, (uint64_t)18104000);
    CHECK_EQ(back.gear, std::string("fsk32r62-r1/3"));
    CHECK(!back.session);

    // Unknowns are empty fields.
    PathEntry bare = entry(T0, "W1AW", std::nullopt, "");
    bare.session = true;
    line = pathLine(bare);
    CHECK_EQ(line, std::string("2026-10-22T00:00:00Z\tW1AW\t\t\t\t\t\tsession"));
    CHECK(parsePathLine(line, back));
    CHECK(!back.snrDb);
    CHECK(back.band.empty() && back.dialHz == 0 && back.session);

    // A contact: worked, not a burst heard.
    PathEntry work = entry(T0, "W1AW", std::nullopt, "20 m");
    work.worked = true;
    line = pathLine(work);
    CHECK_EQ(line, std::string("2026-10-22T00:00:00Z\tW1AW\t\t\t20 m\t\t\twork"));
    CHECK(parsePathLine(line, back));
    CHECK(back.worked && !back.session);
    CHECK(parsePathLine(pathLine(e), back));
    CHECK(!back.worked);

    CHECK(!parsePathLine("", back));
    CHECK(!parsePathLine("2026-10-22T00:00:00Z\tW1AW", back));
    CHECK(!parsePathLine("2026-10-22T00:00:00Z\tW1AW\tloud\t\t\t\t\tD2G", back));
    CHECK(!parsePathLine("2026-10-22T00:00:00Z\tW1AW\t\t\t\t\t\tsomewhere", back));
}

void strip()
{
    std::vector<PathEntry> all;
    // KC2G on 17 m from 2000 to 0000Z on three days, and once on 30 m.
    for (uint64_t day = 0; day < 3; day++)
    {
        for (uint64_t h = 20; h < 24; h++)
            all.push_back(entry(T0 + day * 24 * HOUR + h * HOUR + 600000, "KC2G", -4.0 - (double)day * 2, "17 m"));
    }
    all.push_back(entry(T0 + 3 * HOUR, "KC2G", -12.0, "30 m"));
    // Heard while the dial wasn't read, and another station.
    all.push_back(entry(T0 + 5 * HOUR, "KC2G", std::nullopt, ""));
    all.push_back(entry(T0 + 6 * HOUR, "W1AW", 3.0, "20 m"));
    // Too old for the strip.
    all.push_back(entry(T0 - 20 * 24 * HOUR, "KC2G", -1.0, "40 m"));
    // A contact isn't a burst heard: no 80 m row.
    PathEntry contact = entry(T0 + 7 * HOUR, "KC2G", std::nullopt, "80 m");
    contact.worked = true;
    all.push_back(contact);

    std::vector<const PathEntry*> ptrs;
    for (const PathEntry& e : all) ptrs.push_back(&e);
    uint64_t now = T0 + 3 * 24 * HOUR;
    PathStrip s = pathStrip(ptrs, "KC2G", now, 14);
    CHECK_EQ(s.bands.size(), (size_t)3);
    if (s.bands.size() == 3)
    {
        CHECK_EQ(s.bands[0].band, std::string("30 m"));
        CHECK_EQ(s.bands[1].band, std::string("17 m"));
        CHECK_EQ(s.bands[2].band, std::string(""));
        const BandRow& row = s.bands[1];
        CHECK_EQ(row.bursts, (size_t)12);
        CHECK_EQ(row.days, (size_t)3);
        CHECK_EQ(row.hours[20].bursts, (size_t)3);
        CHECK_EQ(row.hours[20].days, (size_t)3);
        CHECK(row.hours[20].medianDb && *row.hours[20].medianDb == -6.0);
        CHECK_EQ(row.hours[19].bursts, (size_t)0);
        CHECK(row.medianDb && *row.medianDb == -6.0);
        CHECK_EQ(rowSummary(row), std::string("17 m: 2000\xE2\x80\x93" "0000Z on 3 days, median \xE2\x88\x92" "6.0 dB"));
        CHECK_EQ(rowSummary(s.bands[0]),
                 std::string("30 m: 0300\xE2\x80\x93" "0400Z on 1 day, median \xE2\x88\x92" "12.0 dB"));
        CHECK_EQ(rowSummary(s.bands[2]), std::string("Band not known: 0500\xE2\x80\x93" "0600Z on 1 day"));
    }
    CHECK_EQ(s.lastMs, T0 + 2 * 24 * HOUR + 23 * HOUR + 600000);

    // A run over midnight stays whole; more than three runs name the
    // busiest three; every hour is all day.
    BandRow row;
    row.band = "20 m";
    for (int h : {22, 23, 0, 1}) row.hours[(size_t)h].bursts = 1;
    row.days = 2;
    CHECK_EQ(rowSummary(row), std::string("20 m: 2200\xE2\x80\x93" "0200Z on 2 days"));
    row = BandRow();
    row.band = "40 m";
    row.days = 1;
    for (int h : {2, 6, 10, 14, 18}) row.hours[(size_t)h].bursts = (size_t)(h == 6 ? 1 : 5);
    CHECK_EQ(rowSummary(row), std::string("40 m: 0200\xE2\x80\x93" "0300Z, 1000\xE2\x80\x93" "1100Z, 1400\xE2\x80\x93"
                                          "1500Z and other hours on 1 day"));
    for (HourCell& c : row.hours) c.bursts = 1;
    CHECK_EQ(rowSummary(row), std::string("40 m: all day on 1 day"));
}

void file()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "ragtime_radio_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::string path = (dir / "paths.log").string();

    uint64_t now = T0 + 30 * 24 * HOUR;
    {
        PathLog log;
        log.setPath(path, T0);
        // A month of lines, one an hour.
        for (uint64_t at = T0; at < now; at += HOUR) log.add(entry(at, at % (2 * HOUR) == 0 ? "KC2G" : "W1AW", -3.0, "20 m"));
        CHECK(log.problem().empty());
        CHECK_EQ(log.entries().size(), (size_t)(30 * 24));
        log.trim(now);
        CHECK_EQ(log.entries().size(), (size_t)(PathLog::KEEP_DAYS * 24));
    }
    // Appended, never rewritten: a stray line in the middle is skipped.
    {
        std::ofstream more(path, std::ios::app);
        more << "not a line\n";
    }
    {
        PathLog log;
        log.setPath(path, now);
        CHECK_EQ(log.entries().size(), (size_t)(PathLog::KEEP_DAYS * 24));
        CHECK_EQ(log.of("KC2G", 0).size(), (size_t)(PathLog::KEEP_DAYS * 12));
        log.add(entry(now, "KC2G", -9.0, "17 m"));
        CHECK_EQ(log.entries().back().band, std::string("17 m"));
    }
    // The whole file is still there: 30 days and the last line.
    std::vector<PathEntry> everything = readPathTail(path, 0);
    CHECK_EQ(everything.size(), (size_t)(30 * 24 + 1));
    if (!everything.empty()) CHECK_EQ(everything.front().atMs, T0);

    // A file that isn't there reads as empty.
    CHECK(readPathTail((dir / "missing.log").string(), 0).empty());
    fs::remove_all(dir, ec);
}

} // namespace

int main()
{
    bands();
    dialReader();
    vfoMode();
    meters();
    strength();
    filter();
    filterSet();
    filterSetTwice();
    onlyAsks();
    lines();
    strip();
    file();
    return ragtime_test::finish();
}
