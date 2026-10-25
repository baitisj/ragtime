// The flight recorder: lines and frames written as they cross, read back
// the same, files rolled at the day and the size, a week and the cap kept,
// and no secret setting let into a report.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/FlightRecorder.h"

using namespace ragtime;
using link::FlightLine;
using link::FlightRecorder;

namespace
{

// 2026-10-25T16:25:42.123Z
constexpr uint64_t T0 = 1792945542123ull;
constexpr uint64_t DAY = 24ull * 60 * 60 * 1000;

std::filesystem::path freshFolder(const char* name)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

std::string readAll(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<FlightLine> readFolder(const FlightRecorder& recorder)
{
    std::string all;
    for (const std::string& name : recorder.fileNames())
        all += readAll(std::filesystem::path(recorder.folder()) / name);
    return link::parseFlight(all);
}

void testTimes()
{
    CHECK_EQ(link::flightTime(T0), std::string("2026-10-25T16:25:42.123Z"));
    CHECK_EQ(link::flightTime(0), std::string("1970-01-01T00:00:00.000Z"));
    uint64_t ms = 0;
    CHECK(link::parseFlightTime("2026-10-25T16:25:42.123Z", ms));
    CHECK_EQ(ms, T0);
    CHECK(link::parseFlightTime("2026-10-25 16:25", ms));
    CHECK_EQ(ms, T0 - 42123);
    CHECK(link::parseFlightTime("2026-10-25T16:25:42.1", ms));
    CHECK_EQ(ms, T0 - 23);
    CHECK(link::parseFlightTime("2024-02-29T00:00:00Z", ms));
    CHECK_EQ(link::flightTime(ms), std::string("2024-02-29T00:00:00.000Z"));
    CHECK(!link::parseFlightTime("2026-13-01T00:00", ms));
    CHECK(!link::parseFlightTime("2026-10-25", ms));
    CHECK(!link::parseFlightTime("2026-10-25T16:25x", ms));
}

void testEscapes()
{
    std::string line = std::string("BCAST 2 HEARD KC2G \\ ") + '\x01' + "\xc3\xa9";
    std::string escaped = link::escapeCommandLine(line);
    CHECK_EQ(escaped, std::string("BCAST 2 HEARD KC2G \\\\ \\x01\\xc3\\xa9"));
    CHECK_EQ(link::unescapeCommandLine(escaped), line);
}

void testRecordAndRead()
{
    std::filesystem::path dir = freshFolder("ragtime-flight-test-read");
    FlightRecorder recorder;
    recorder.setFolder(dir.string(), T0);
    recorder.up("127.0.0.1 KISS 8100 commands 8300", T0 + 1);
    // Our lines go whole; the host's arrive in pieces, ending in CR or LF.
    recorder.commandOut("MYCALL AG7EW\rCHAT ON\r", 21, T0 + 2);
    recorder.commandIn("O", 1, T0 + 5);
    recorder.commandIn("K\rBCAST PO", 10, T0 + 6);
    recorder.commandIn("RT 2\r\n", 6, T0 + 7);
    // A frame in two pieces, and a second one in the same read.
    const uint8_t a[] = {0xC0, 0x20, 0xA0, 0xDB};
    const uint8_t b[] = {0xDC, 0xC0, 0xC0, 0x20, 0x01, 0xC0};
    recorder.kissIn(a, sizeof a, T0 + 10);
    recorder.kissIn(b, sizeof b, T0 + 11);
    const uint8_t ours[] = {0xC0, 0x20, 0xA0, 0x05, 0xC0};
    recorder.kissOut(ours, sizeof ours, T0 + 12);
    recorder.dataUp(true, T0 + 13);
    recorder.dataOut(312, T0 + 14);
    recorder.dataIn(40, T0 + 15);
    recorder.dataIn(0, T0 + 15);    // nothing: no line
    recorder.dataUp(false, T0 + 16);
    // A half line at the drop is kept.
    recorder.commandIn("BUSY O", 6, T0 + 17);
    recorder.down("data2g-host closed its command port", T0 + 18);

    std::vector<std::string> names = recorder.fileNames();
    CHECK_EQ(names.size(), (size_t)1);
    CHECK_EQ(names.empty() ? std::string() : names[0], std::string("flight-20261025-162542-123.log"));

    std::vector<FlightLine> lines = readFolder(recorder);
    using K = FlightLine::Kind;
    // The file's own note, then "opened", then the rest.
    CHECK_EQ(lines.size(), (size_t)16);
    if (lines.size() != 16) return;
    CHECK(lines[0].kind == K::Note);
    CHECK(lines[1].kind == K::Note && lines[1].text.find("opened") != std::string::npos);
    CHECK(lines[2].kind == K::Up && lines[2].text == "127.0.0.1 KISS 8100 commands 8300");
    CHECK_EQ(lines[2].ms, T0 + 1);
    CHECK(lines[3].kind == K::CommandOut && lines[3].text == "MYCALL AG7EW");
    CHECK(lines[4].kind == K::CommandOut && lines[4].text == "CHAT ON");
    CHECK(lines[5].kind == K::CommandIn && lines[5].text == "OK");
    CHECK_EQ(lines[5].ms, T0 + 6);
    CHECK(lines[6].kind == K::CommandIn && lines[6].text == "BCAST PORT 2");
    CHECK_EQ(lines[6].ms, T0 + 7);
    CHECK(lines[7].kind == K::KissIn && lines[7].bytes == std::vector<uint8_t>({0x20, 0xA0, 0xDB, 0xDC}));
    CHECK_EQ(lines[7].ms, T0 + 11);
    CHECK(lines[8].kind == K::KissIn && lines[8].bytes == std::vector<uint8_t>({0x20, 0x01}));
    CHECK(lines[9].kind == K::KissOut && lines[9].bytes == std::vector<uint8_t>({0x20, 0xA0, 0x05}));
    CHECK(lines[10].kind == K::DataUp);
    CHECK(lines[11].kind == K::DataOut && lines[11].count == 312);
    CHECK(lines[12].kind == K::DataIn && lines[12].count == 40);
    CHECK(lines[13].kind == K::DataDown);
    CHECK(lines[14].kind == K::CommandIn && lines[14].text == "BUSY O");
    CHECK(lines[15].kind == K::Down && lines[15].text == "data2g-host closed its command port");
    CHECK_EQ(lines[15].ms, T0 + 18);

    // The text itself, as a person reads it.
    std::string text = readAll(dir / names[0]);
    CHECK(text.find("2026-10-25T16:25:42.125Z cmd> MYCALL AG7EW\n") != std::string::npos);
    CHECK(text.find("kiss< 20a0dbdc\n") != std::string::npos);
    CHECK(text.find("data> 312\n") != std::string::npos);

    // Lines a reader doesn't know are skipped.
    FlightLine line;
    CHECK(!link::parseFlightLine("2026-10-25T16:25:42.123Z warp 9", line));
    CHECK(!link::parseFlightLine("2026-10-25T16:25:42.123Z kiss< 2", line));
    CHECK(!link::parseFlightLine("", line));

    // Off: nothing written.
    recorder.setFolder("", T0 + 20);
    recorder.commandIn("OK\r", 3, T0 + 21);
    CHECK_EQ(readAll(dir / names[0]), text);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testRolling()
{
    std::filesystem::path dir = freshFolder("ragtime-flight-test-roll");
    FlightRecorder recorder;
    recorder.setFolder(dir.string(), T0);
    // A new file at the UTC day.
    uint64_t midnight = (T0 / DAY + 1) * DAY;
    recorder.commandIn("OK\r", 3, midnight - 1);
    recorder.commandIn("OK\r", 3, midnight + 5);
    CHECK_EQ(recorder.fileNames().size(), (size_t)2);

    // And at the size.
    recorder.setLimits(200, 1000000);
    for (int i = 0; i < 10; i++) recorder.commandIn("BUSY ON\r", 8, midnight + 1000 * (uint64_t)(i + 1));
    size_t files = recorder.fileNames().size();
    CHECK(files >= 4);
    for (const std::string& name : recorder.fileNames())
    {
        std::error_code ec;
        CHECK(std::filesystem::file_size(dir / name, ec) < 400);
    }

    // The cap: the oldest go, never the open file.
    recorder.setLimits(200, 500);
    for (int i = 0; i < 5; i++) recorder.commandIn("BUSY OFF\r", 9, midnight + 20000 + 1000 * (uint64_t)i);
    uint64_t total = 0;
    for (const std::string& name : recorder.fileNames())
    {
        std::error_code ec;
        total += std::filesystem::file_size(dir / name, ec);
    }
    CHECK(total <= 500 + 200);
    CHECK(recorder.fileNames().size() < files);
    std::vector<FlightLine> lines = readFolder(recorder);
    CHECK(!lines.empty() && lines.back().text == "BUSY OFF");

    // The week: a file from the 25th goes once the 1st of November is out.
    recorder.setLimits(FlightRecorder::FILE_BYTES, FlightRecorder::CAP_BYTES);
    std::filesystem::path old = dir / "flight-20261020-080000-000.log";
    std::ofstream(old) << "2026-10-20T08:00:00.000Z cmd< OK\n";
    std::filesystem::path stranger = dir / "notes.txt";
    std::ofstream(stranger) << "mine\n";
    const uint64_t oldDayEnd = 1792540800000ull;   // 2026-10-21T00:00Z
    recorder.prune(oldDayEnd + FlightRecorder::KEEP_MS - 1);
    CHECK(std::filesystem::exists(old));
    recorder.prune(oldDayEnd + FlightRecorder::KEEP_MS);
    CHECK(!std::filesystem::exists(old));
    CHECK(std::filesystem::exists(stranger));
    // Windows won't remove a file still open: close the recorder's first.
    recorder.setFolder("", midnight + 30000);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testUnwritableFolder()
{
    std::filesystem::path dir = freshFolder("ragtime-flight-test-blocked");
    // A file where the folder should be: recording fails quietly.
    std::ofstream(dir) << "in the way\n";
    FlightRecorder recorder;
    recorder.setFolder((dir / "flight").string(), T0);
    recorder.commandIn("OK\r", 3, T0 + 1);
    CHECK(recorder.fileNames().empty());
    std::error_code ec;
    std::filesystem::remove(dir, ec);
}

void testReportableSettings()
{
    CHECK(link::reportableSetting("/Station/Callsign"));
    CHECK(link::reportableSetting("/Files/AutoAccept"));
    CHECK(link::reportableSetting("/Broadcast/Seals/AG7EW"));
    CHECK(!link::reportableSetting("/Station/PrivateKey"));
    CHECK(!link::reportableSetting("/Keys/Ed25519Secret"));
    CHECK(!link::reportableSetting("/Station/RecoveryWords"));
    CHECK(!link::reportableSetting("/Mesh/Password"));
}

} // namespace

int main()
{
    testTimes();
    testEscapes();
    testRecordAndRead();
    testRolling();
    testUnwritableFolder();
    testReportableSettings();
    return ragtime_test::finish();
}
