// BroadcastLog, the Broadcast tab's lasting log: its lines read back as
// rows with a rule after each run of Ragtime, text with tabs and newlines
// in it, a run cut short, file lines and how they ended, the newest rows
// only, lines it doesn't know, and the file cut back once it grows too big.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastLog.h"

using namespace ragtime;
using broadcast::BroadcastLog;
using broadcast::Message;
using Kind = Message::Kind;
using MState = Message::State;

namespace
{

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC

Message text(uint64_t id, bool ours, const std::string& from, const std::string& t, uint64_t atMs,
             MState state = MState::Heard)
{
    Message m;
    m.id = id;
    m.ours = ours;
    m.from = from;
    m.text = t;
    m.atMs = atMs;
    m.time = (uint32_t)(atMs / 1000);
    m.seq = (uint32_t)id;
    m.state = state;
    return m;
}

Message fileCard(uint64_t id, bool ours, const std::string& from, const std::string& name, uint64_t atMs)
{
    Message m;
    m.id = id;
    m.kind = Kind::File;
    m.file = 100 + id;
    m.ours = ours;
    m.from = from;
    m.text = name;
    m.atMs = atMs;
    return m;
}

std::filesystem::path scratch(const std::string& name)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("ragtime-" + name + "-test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

std::string readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

//-------------------------------------------------------------------------

void testTwoRunsAndTheirRules()
{
    std::filesystem::path dir = scratch("broadcast-log");
    std::string path = (dir / "broadcast.log").string();
    BroadcastLog log(path);
    CHECK(log.load(100).empty());   // nothing yet, and no folder made
    CHECK(!std::filesystem::exists(dir));

    // The first run: a CQ of ours, an answer, an addressed one acknowledged,
    // a file heard and saved, and one of ours still going when it closed.
    log.began(T0);
    Message cq = text(1, true, "AG7EW", "CQ CQ de AG7EW", T0 + 1000, MState::Queued);
    cq.mode = "qpsk-r1/3";
    log.added(cq);
    cq.state = MState::Sent;
    cq.mode = "fsk16r25-r1/3";
    log.settled(cq);
    log.added(text(2, false, "W1AW", "QST de W1AW:\tthe net\nat 0100Z, C:\\net", T0 + 5000));
    Message addressed = text(3, true, "AG7EW", "gm Hiram", T0 + 9000, MState::Queued);
    addressed.to = "W1AW";
    addressed.ackWanted = true;
    log.added(addressed);
    addressed.state = MState::Acknowledged;
    log.settled(addressed);
    Message card = fileCard(4, false, "W1AW", "bulletin.txt", T0 + 12000);
    log.added(card);
    log.fileEnded(card, "Saved");
    log.added(text(5, true, "AG7EW", "still going", T0 + 13000, MState::Sending));
    log.ended(T0 + 60 * 60 * 1000);

    // The second run, cut short: no E.
    log.began(T0 + 2 * 60 * 60 * 1000);
    log.added(text(1, false, "KC2G", "QRV", T0 + 2 * 60 * 60 * 1000 + 5000));
    log.added(fileCard(2, true, "AG7EW", "map.png", T0 + 2 * 60 * 60 * 1000 + 6000));

    std::vector<Message> rows = log.load(100);
    CHECK_EQ(rows.size(), (size_t)9);
    if (rows.size() == 9)
    {
        CHECK(rows[0].past && rows[0].ours && rows[0].state == MState::Sent);
        CHECK_EQ(rows[0].mode, std::string("fsk16r25-r1/3"));
        CHECK_EQ(rows[0].seq, (uint32_t)1);
        CHECK_EQ(rows[0].time, (uint32_t)((T0 + 1000) / 1000));
        CHECK_EQ(rows[1].text, std::string("QST de W1AW:\tthe net\nat 0100Z, C:\\net"));
        CHECK(rows[1].state == MState::Heard && !rows[1].ours);
        CHECK(rows[2].state == MState::Acknowledged && rows[2].ackWanted);
        CHECK_EQ(rows[2].to, std::string("W1AW"));
        CHECK(rows[3].kind == Kind::File && rows[3].file == 0);
        CHECK_EQ(rows[3].text, std::string("bulletin.txt"));
        CHECK_EQ(rows[3].outcome, std::string("Saved"));
        CHECK(rows[4].state == MState::NotSent);    // still Sending when it closed
        CHECK(rows[5].kind == Kind::Rule);
        CHECK_EQ(rows[5].atMs, T0 + 60 * 60 * 1000);
        CHECK_EQ(rows[6].from, std::string("KC2G"));
        CHECK(rows[7].kind == Kind::File && rows[7].ours && rows[7].outcome.empty());
        // The run cut short ends at its last row.
        CHECK(rows[8].kind == Kind::Rule);
        CHECK_EQ(rows[8].atMs, T0 + 2 * 60 * 60 * 1000 + 6000);
    }

    // The newest rows only, and never a rule on top.
    rows = log.load(4);
    CHECK_EQ(rows.size(), (size_t)3);
    if (!rows.empty()) CHECK_EQ(rows.front().from, std::string("KC2G"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testOddLines()
{
    // Lines it doesn't know, broken numbers, a state for a row it never
    // saw, and runs with no rows draw no rule.
    std::string text = "ragtime broadcast log 1\n"
                       "B\t1000\n"
                       "E\t2000\n"
                       "B\t3000\n"
                       "X\twhatever comes later\n"
                       "M\t1\t3500\t3\t7\t0\tH\tW1AW\t\t0\t\thello\r\n"
                       "M\tnope\t3500\t3\t7\t0\tH\tW1AW\t\t0\t\tbroken\n"
                       "S\t9\tS\t\n"
                       "R\t1\tSaved\n"
                       "E\t4000\n"
                       "B\t5000\n";
    std::vector<Message> rows = broadcast::parseBroadcastLog(text, 100);
    CHECK_EQ(rows.size(), (size_t)2);
    if (rows.size() == 2)
    {
        CHECK_EQ(rows[0].text, std::string("hello"));
        CHECK(rows[0].outcome.empty());             // R names files only
        CHECK(rows[1].kind == Kind::Rule && rows[1].atMs == 4000);
    }
    CHECK(broadcast::parseBroadcastLog("", 100).empty());
}

void testCutBack()
{
    std::filesystem::path dir = scratch("broadcast-log-cut");
    std::string path = (dir / "broadcast.log").string();
    BroadcastLog log(path);
    std::string filler(200, 'x');

    // Many runs: cut back at a run's start in the later half.
    uint64_t t = T0;
    uint64_t id = 1;
    while (std::filesystem::exists(path) == false || std::filesystem::file_size(path) <= BroadcastLog::MAX_FILE_BYTES)
    {
        log.began(t);
        for (int i = 0; i < 50; i++) log.added(text(id++, false, "W1AW", filler, t += 1000));
        log.ended(t += 1000);
    }
    uint64_t before = std::filesystem::file_size(path);
    log.began(t += 1000);
    uint64_t after = std::filesystem::file_size(path);
    CHECK(after < before / 2 + 20000);
    CHECK(after > before / 2 - 20000);
    std::string kept = readAll(path);
    CHECK_EQ(kept.compare(0, 24, "ragtime broadcast log 1\n"), 0);
    CHECK_EQ(kept.compare(24, 2, "B\t"), 0);
    CHECK(!log.load(100000).empty());

    // One long run: cut back at a row in the later half.
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    log.began(t);
    while (std::filesystem::file_size(path) <= BroadcastLog::MAX_FILE_BYTES)
        log.added(text(id++, false, "W1AW", filler, t += 1000));
    before = std::filesystem::file_size(path);
    log.began(t += 1000);
    after = std::filesystem::file_size(path);
    CHECK(after < before / 2 + 1000);
    kept = readAll(path);
    CHECK_EQ(kept.compare(24, 2, "M\t"), 0);
    std::vector<Message> rows = log.load(100000);
    CHECK(rows.size() > 1000);
    CHECK(rows.back().kind == Kind::Rule);

    std::filesystem::remove_all(dir, ec);
}

// A line repaired goes above the row it was put before, in the file as
// on the screen.
void testRepairedInPlace()
{
    std::filesystem::path dir = scratch("broadcast-log-place");
    std::string path = (dir / "broadcast.log").string();
    BroadcastLog log(path);
    log.began(T0);
    log.added(text(1, false, "AG7EW", "one", T0 + 1000));
    log.added(text(2, false, "KC2G", "hello", T0 + 2000));
    log.added(text(4, false, "AG7EW", "three", T0 + 3000));
    // "two" came again after its seal: above "three", below "hello".
    log.added(text(3, false, "AG7EW", "two", T0 + 60000), 4);
    log.added(text(5, false, "KC2G", "bye", T0 + 61000));
    log.ended(T0 + 62000);
    std::vector<Message> rows = log.load(100);
    CHECK_EQ(rows.size(), (size_t)6);
    if (rows.size() == 6)
    {
        CHECK_EQ(rows[0].text, std::string("one"));
        CHECK_EQ(rows[1].text, std::string("hello"));
        CHECK_EQ(rows[2].text, std::string("two"));
        CHECK_EQ(rows[3].text, std::string("three"));
        CHECK_EQ(rows[4].text, std::string("bye"));
        CHECK(rows[5].kind == Kind::Rule);
    }
    // A reader from before ignores the extra field: the old order, nothing lost.
    std::string old = readAll(path);
    CHECK(old.find("\ttwo\t4\n") != std::string::npos);
    std::filesystem::remove_all(dir);
}

void testNoPath()
{
    BroadcastLog log;
    log.began(T0);
    log.added(text(1, true, "AG7EW", "nowhere", T0));
    log.ended(T0 + 1000);
    CHECK(log.load(100).empty());
}

} // namespace

int main()
{
    testTwoRunsAndTheirRules();
    testOddLines();
    testCutBack();
    testRepairedInPlace();
    testNoPath();
    return ragtime_test::finish();
}
