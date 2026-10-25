// The radio's side: the bands and the US data segments, the rigctld dial
// reader (it only ever asks "f", stops asking while keyed, and lets a
// reading go stale), and the path log (its lines, the file read back from
// the end, and the 24-hour strip and its words).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/radio/Bands.h"
#include "ragtime/radio/DialReader.h"
#include "ragtime/radio/PathLog.h"

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
    CHECK(usDataRefusal(5357000).find("60 m") != std::string::npos);
    CHECK(!usDataRefusal(0).empty());
}

void dialReader()
{
    DialReader dial;
    uint64_t t = T0;
    CHECK(!dial.dialHz(t));
    dial.tick(t);
    CHECK_EQ(dial.takeOutput(), std::string(""));

    dial.connected(t);
    CHECK_EQ(dial.takeOutput(), std::string("f\n"));
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

    // Keyed: no asking, and the reading stands however long the burst.
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

    // Lines that aren't frequencies are ignored.
    dial.bytes("garbage\n1e9\n.\n0\n", 17, t + 31100);
    CHECK(dial.dialHz(t + 31100) == std::optional<uint64_t>(14105000));

    dial.disconnected();
    CHECK(!dial.dialHz(t + 31200));
    dial.tick(t + 40000);
    CHECK_EQ(dial.takeOutput(), std::string(""));
}

// Every line Ragtime could ever write to rigctld is a frequency query.
void onlyAsksFrequency()
{
    DialReader dial;
    std::string all;
    uint64_t t = T0;
    dial.connected(t);
    for (int i = 0; i < 200; i++)
    {
        t += 250;
        dial.tick(t);
        if (i % 7 == 0) dial.bytes("RPRT -1\n", 8, t);
        if (i % 5 == 0) dial.bytes("14070000\n", 9, t);
        if (i % 40 == 0) dial.setKeyed(i % 80 == 0, t);
        all += dial.takeOutput();
    }
    CHECK(!all.empty());
    for (size_t at = 0; at < all.size(); at += 2) CHECK_EQ(all.substr(at, 2), std::string("f\n"));
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
    onlyAsksFrequency();
    lines();
    strip();
    file();
    return ragtime_test::finish();
}
