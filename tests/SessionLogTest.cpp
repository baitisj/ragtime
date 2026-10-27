// SessionLog, the Session tab's lasting log: its lines read back as rows
// with a rule after each session, text with tabs and newlines in it, a
// session cut short, the newest rows only, lines it doesn't know, a file
// per station, and the file cut back once it grows too big.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/session/SessionChat.h"
#include "ragtime/session/SessionLog.h"

using namespace ragtime;
using session::Message;
using session::SessionLog;
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

void testTwoSessionsAndTheirRules()
{
    std::filesystem::path dir = scratch("session-log");
    SessionLog log(dir.string());
    CHECK(log.load("W1AW", 100).empty());   // nothing yet, and no folder made
    CHECK(!std::filesystem::exists(dir));

    // The first session: ours delivered, theirs, ours never acknowledged.
    log.began("W1AW", T0);
    Message hello = text(1, true, "AG7EW", "Good evening Hiram", T0 + 1000, MState::Queued);
    log.added("W1AW", hello);
    hello.state = MState::Sent;
    log.settled("W1AW", hello);
    hello.state = MState::Delivered;
    log.settled("W1AW", hello);
    log.added("W1AW", text(2, false, "W1AW", "Fine business\tJeff,\nhow copy? C:\\path", T0 + 5000));
    Message lost = text(3, true, "AG7EW", "73", T0 + 9000, MState::Sent);
    log.added("W1AW", lost);
    log.ended("W1AW", T0 + 14 * 60 * 1000);
    // A file that went in it.
    Message file;
    file.kind = Kind::File;
    file.ours = true;
    file.from = "AG7EW";
    file.text = "antenna.jpg, 12 KB: Sent";
    file.atMs = T0 + 15 * 60 * 1000;
    file.path = "/tmp/sent pictures/antenna.j2k";
    file.picture = true;
    log.file("W1AW", file);

    // The second, with ids that start again (a restart).
    log.began("W1AW", T0 + 60 * 60 * 1000);
    log.added("W1AW", text(1, false, "W1AW", "Back again", T0 + 61 * 60 * 1000));
    log.ended("W1AW", T0 + 62 * 60 * 1000);

    CHECK(std::filesystem::exists(dir / "W1AW.log"));
    CHECK_EQ(log.pathFor("W1AW"), (dir / "W1AW.log").string());
    std::vector<Message> rows = log.load("W1AW", 100);
    CHECK_EQ(rows.size(), (size_t)7);
    if (rows.size() == 7)
    {
        CHECK(rows[0].kind == Kind::Text && rows[0].ours && rows[0].from == "AG7EW");
        CHECK(rows[0].text == "Good evening Hiram" && rows[0].state == MState::Delivered);
        CHECK_EQ(rows[0].atMs, T0 + 1000);
        CHECK_EQ(rows[0].seq, (uint32_t)1);
        CHECK_EQ(rows[0].time, (uint32_t)((T0 + 1000) / 1000));
        CHECK(rows[1].text == "Fine business\tJeff,\nhow copy? C:\\path" && !rows[1].ours);
        CHECK(rows[1].state == MState::Heard);
        // Still Sent when its session ended: it wasn't delivered.
        CHECK(rows[2].text == "73" && rows[2].state == MState::NotDelivered);
        CHECK(rows[3].kind == Kind::Rule);
        CHECK_EQ(rows[3].beganMs, T0);
        CHECK_EQ(rows[3].atMs, T0 + 14 * 60 * 1000);
        CHECK(rows[4].kind == Kind::File && rows[4].ours && rows[4].text == "antenna.jpg, 12 KB: Sent");
        CHECK(rows[4].path == "/tmp/sent pictures/antenna.j2k" && rows[4].picture);
        CHECK(rows[0].path.empty() && !rows[0].picture);
        CHECK(rows[5].text == "Back again" && rows[5].state == MState::Heard);
        CHECK(rows[6].kind == Kind::Rule && rows[6].atMs == T0 + 62 * 60 * 1000);
        bool allPast = true, noIds = true;
        for (const Message& m : rows)
        {
            allPast = allPast && m.past;
            noIds = noIds && m.id == 0;
        }
        CHECK(allPast && noIds);
    }

    // The newest rows only, never starting on a rule.
    std::vector<Message> few = log.load("W1AW", 4);
    CHECK_EQ(few.size(), (size_t)3);
    if (few.size() == 3) CHECK(few[0].kind == Kind::File && few[2].kind == Kind::Rule);

    // A station each; a portable call made safe for a file name.
    log.began("TEST1/P", T0);
    log.added("TEST1/P", text(1, false, "TEST1/P", "hi", T0));
    CHECK(std::filesystem::exists(dir / "TEST1_P.log"));
    CHECK_EQ(log.load("TEST1/P", 100).size(), (size_t)2);  // its message, and the rule of a session cut short
    CHECK_EQ(log.load("W1AW", 100).size(), (size_t)7);
    CHECK_EQ(session::sessionLogName("VE7/W1AW/P"), std::string("VE7_W1AW_P"));

    // No folder keeps nothing.
    SessionLog none;
    none.began("W1AW", T0);
    none.added("W1AW", text(1, false, "W1AW", "lost", T0));
    CHECK(none.load("W1AW", 100).empty());
    CHECK(none.pathFor("W1AW").empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testLinesCutShortOrUnknown()
{
    std::string text = "ragtime session log 1\n"
                       "B\t1000\n"
                       "M\t7\t1100\t1\t1\t1\tS\tAG7EW\tcut short\n"
                       "S\t8\tD\n"                          // no such message: nothing
                       "X\tsomething from a later Ragtime\n"
                       "M\tnot a number\t1\t1\t1\t1\tH\tW1AW\tbad\n"
                       "M\t9\t1200\t1\t2\t0\tH\tW1AW\n"      // too few fields
                       "\n"
                       "B\t5000\n"                          // the app stopped under the first
                       "B\t6000\n"                          // and this one said nothing
                       "E\t7000\n"
                       "M\t1\t8000\t8\t1\t0\tH\tW1AW\tafter a compact\r\n";
    std::vector<Message> rows = session::parseSessionLog(text, 100);
    CHECK_EQ(rows.size(), (size_t)4);
    if (rows.size() == 4)
    {
        CHECK(rows[0].text == "cut short" && rows[0].state == MState::NotDelivered);
        // A session cut short ends at its last row.
        CHECK(rows[1].kind == Kind::Rule && rows[1].beganMs == 1000 && rows[1].atMs == 1100);
        // An empty session draws no rule; a message with no B opens one.
        CHECK(rows[2].text == "after a compact" && rows[2].from == "W1AW");
        CHECK(rows[3].kind == Kind::Rule && rows[3].beganMs == 0 && rows[3].atMs == 8000);
    }
    CHECK(session::parseSessionLog("", 100).empty());
    CHECK(session::parseSessionLog("garbage", 100).empty());
}

void testTheFileIsCutBack()
{
    std::filesystem::path dir = scratch("session-log-compact");
    SessionLog log(dir.string());
    std::string filler(1000, 'x');
    uint64_t at = T0;
    int sessions = 0;
    while (true)
    {
        std::error_code ec;
        uintmax_t size = std::filesystem::file_size(dir / "W1AW.log", ec);
        if (!ec && size > SessionLog::MAX_FILE_BYTES) break;
        log.began("W1AW", at);
        for (int i = 0; i < 10; i++) log.added("W1AW", text((uint64_t)i + 1, false, "W1AW", filler, at += 1000));
        log.ended("W1AW", at += 1000);
        sessions++;
    }
    size_t before = log.load("W1AW", 1000000).size();
    CHECK_EQ(before, (size_t)sessions * 11);
    // The next session's start cuts it to its later half, at a session's start.
    log.began("W1AW", at += 1000);
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(dir / "W1AW.log", ec);
    CHECK(!ec && size <= SessionLog::MAX_FILE_BYTES / 2 + 20000 && size >= SessionLog::MAX_FILE_BYTES / 2 - 20000);
    std::string kept = readAll(log.pathFor("W1AW"));
    CHECK(kept.rfind("ragtime session log 1\nB\t", 0) == 0);
    std::vector<Message> rows = log.load("W1AW", 1000000);
    CHECK(!rows.empty() && rows.size() % 11 == 0 && rows.size() < before);
    CHECK(!std::filesystem::exists(dir / "W1AW.log.new"));
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main()
{
    testTwoSessionsAndTheirRules();
    testLinesCutShortOrUnknown();
    testTheFileIsCutBack();
    return ragtime_test::finish();
}
