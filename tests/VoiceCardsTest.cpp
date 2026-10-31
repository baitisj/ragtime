// Voice cards (docs/STAGE1.md, "Voice cards"): the play queue (what plays
// by itself while the deck is open, play pressed, Stop, Push to talk's
// pause), and a voice message kept by the chat logs and the Files tab's
// log so its card is there after a restart.
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastLog.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/session/SessionLog.h"
#include "ragtime/voice/PlayQueue.h"

using namespace ragtime;
using voice::PlayQueue;

namespace
{

const uint64_t T0 = 1793462400000; // 2026-10-31 00:00 UTC

std::filesystem::path scratch(const std::string& name)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("ragtime-" + name + "-test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

// Voice that arrives while the deck is open plays in the order it came,
// each clip once; with the deck closed it waits for a click.
void testArrivalOrder()
{
    PlayQueue q;
    auto first = q.arrived("b1", true);
    CHECK(first.has_value() && first->key == "b1" && first->fromFrame == 0);
    CHECK(!q.arrived("s2", true).has_value());   // waits: b1 plays
    CHECK(!q.arrived("b3", true).has_value());
    CHECK_EQ(q.playing(), std::string("b1"));
    CHECK(q.waiting("s2") && q.waiting("b3"));
    CHECK_EQ(q.waitingCount(), (size_t)2);
    // A clip arrives once: the same key again changes nothing.
    CHECK(!q.arrived("s2", true).has_value());
    CHECK_EQ(q.waitingCount(), (size_t)2);
    // A late word from a clip that isn't playing changes nothing.
    CHECK(!q.finished("b3").has_value());
    auto second = q.finished("b1");
    CHECK(second.has_value() && second->key == "s2");
    auto third = q.finished("s2");
    CHECK(third.has_value() && third->key == "b3");
    CHECK(!q.finished("b3").has_value());
    CHECK(q.playing().empty());
    // b1 played already: it doesn't come round again.
    CHECK(!q.arrived("b1", true).has_value());

    // Deck closed: arrivals wait for a click, and are never queued.
    PlayQueue closed;
    CHECK(!closed.arrived("b9", false).has_value());
    CHECK(closed.playing().empty() && !closed.waiting("b9"));
    // Pressed later, it plays.
    PlayQueue::Start s = closed.play("b9");
    CHECK(s.key == "b9" && closed.playing() == "b9");
}

// Play pressed stops what plays and plays at once; the queue waits after
// it. A click on the strip plays from that frame. Stop empties the queue.
void testPlayAndStop()
{
    PlayQueue q;
    q.arrived("b1", true);
    q.arrived("b2", true);
    q.arrived("b3", true);
    PlayQueue::Start s = q.play("p/old.sot", 250);
    CHECK(s.key == "p/old.sot" && s.fromFrame == 250);
    CHECK_EQ(q.playing(), std::string("p/old.sot"));
    // b1 was cut off; b2 and b3 still wait, after the one asked for.
    CHECK(!q.waiting("b1") && q.waiting("b2") && q.waiting("b3"));
    // b1's own late finish changes nothing now.
    CHECK(!q.finished("b1").has_value());
    auto next = q.finished("p/old.sot");
    CHECK(next.has_value() && next->key == "b2");
    // Playing one that waits takes it out of the queue.
    q.play("b3");
    CHECK(!q.waiting("b3") && q.waitingCount() == 0);
    // Stop: quiet, nothing waiting.
    q.arrived("b4", true);
    CHECK(q.waiting("b4"));
    q.stop();
    CHECK(q.playing().empty() && q.waitingCount() == 0);
    CHECK(!q.finished("b3").has_value());
}

// Push to talk pauses what plays; letting go carries it on from there.
// Arrivals while it's held wait; with nothing paused, release starts them.
void testPushToTalkHolds()
{
    PlayQueue q;
    q.arrived("b1", true);
    CHECK(q.hold(120));
    CHECK(q.held() && q.paused() && q.playing() == "b1");
    // The sound stopped for the pause reports finished(false): not the end.
    CHECK(!q.finished("b1").has_value());
    CHECK(!q.arrived("b2", true).has_value());
    CHECK(!q.hold(10));   // held already
    auto resume = q.release();
    CHECK(resume.has_value() && resume->key == "b1" && resume->fromFrame == 120);
    CHECK(!q.held());
    auto next = q.finished("b1");
    CHECK(next.has_value() && next->key == "b2");
    q.finished("b2");

    // Held with nothing playing: arrivals wait, release starts the first.
    CHECK(!q.hold(0));
    CHECK(q.held() && !q.paused());
    CHECK(!q.arrived("b5", true).has_value());
    CHECK(!q.arrived("b6", true).has_value());
    auto first = q.release();
    CHECK(first.has_value() && first->key == "b5");
    CHECK(q.waiting("b6"));

    // Play pressed while held: it plays (the operator asked).
    PlayQueue r;
    r.hold(0);
    PlayQueue::Start s = r.play("b7");
    CHECK(s.key == "b7" && r.playing() == "b7");
    CHECK(!r.release().has_value() || r.playing() == "b7");
}

// The deck closing empties the queue but lets the clip playing finish;
// a card removed neither plays nor waits.
void testClearAndForget()
{
    PlayQueue q;
    q.arrived("b1", true);
    q.arrived("b2", true);
    q.arrived("b3", true);
    q.clearWaiting();
    CHECK(q.playing() == "b1" && q.waitingCount() == 0);
    CHECK(!q.finished("b1").has_value());

    q.arrived("b4", true);
    q.arrived("b5", true);
    CHECK(!q.forget("b5"));
    CHECK(!q.waiting("b5"));
    CHECK(q.forget("b4"));
    CHECK(q.playing().empty());
}

// A voice card on the Broadcast tab is still there after a restart: its
// R line names the file and that it is voice. A picture likewise; a plain
// file, or one not kept, has no path.
void testBroadcastLogKeepsVoice()
{
    std::filesystem::path dir = scratch("voice-broadcast-log");
    broadcast::BroadcastLog log((dir / "broadcast.log").string());
    log.began(T0);
    auto card = [](uint64_t id, bool ours, const std::string& from, const std::string& name, uint64_t at) {
        broadcast::Message m;
        m.id = id;
        m.kind = broadcast::Message::Kind::File;
        m.file = 100 + id;
        m.ours = ours;
        m.from = from;
        m.text = name;
        m.atMs = at;
        return m;
    };
    broadcast::Message heard = card(1, false, "KC2G", "hello.sot", T0 + 1000);
    log.added(heard);
    heard.voice = true;
    log.fileEnded(heard, "Saved", "/home/op/Ragtime voice/hello.sot");
    broadcast::Message mine = card(2, true, "AG7EW", "shack.j2k", T0 + 2000);
    log.added(mine);
    mine.picture = true;
    log.fileEnded(mine, "Sent", "/home/op/sent/shack.j2k");
    broadcast::Message plain = card(3, false, "W1AW", "notes.txt", T0 + 3000);
    log.added(plain);
    log.fileEnded(plain, "Ignored");
    log.ended(T0 + 4000);

    std::vector<broadcast::Message> rows = log.load(100);
    CHECK_EQ(rows.size(), (size_t)4);
    if (rows.size() == 4)
    {
        CHECK(rows[0].kind == broadcast::Message::Kind::File && rows[0].past);
        CHECK(rows[0].voice && !rows[0].picture);
        CHECK_EQ(rows[0].path, std::string("/home/op/Ragtime voice/hello.sot"));
        CHECK_EQ(rows[0].outcome, std::string("Saved"));
        CHECK(rows[1].picture && !rows[1].voice && rows[1].path == "/home/op/sent/shack.j2k");
        CHECK(!rows[2].picture && !rows[2].voice && rows[2].path.empty());
        CHECK_EQ(rows[2].outcome, std::string("Ignored"));
        CHECK(rows[3].kind == broadcast::Message::Kind::Rule);
    }

    // BroadcastChat marks the live card when its file ends, so the log
    // line says voice.
    link::HostLink host{link::HostLink::Settings{}};
    broadcast::BroadcastChat chat(host);
    std::filesystem::path dir2 = scratch("voice-broadcast-chat");
    broadcast::BroadcastLog log2((dir2 / "broadcast.log").string());
    chat.setLog(&log2, T0);
    chat.addFile(7, false, "KC2G", "cq.sot", T0 + 1000);
    chat.fileEnded(7, "Saved", "/tmp/cq.sot", false, true);
    CHECK(!chat.messages().empty() && chat.messages().back().voice);
    std::vector<broadcast::Message> again = log2.load(100);
    bool kept = false;
    for (const broadcast::Message& m : again) kept = kept || (m.voice && m.path == "/tmp/cq.sot");
    CHECK(kept);
}

// A session's voice message is one F line with v for its kind.
void testSessionLogKeepsVoice()
{
    std::filesystem::path dir = scratch("voice-session-log");
    session::SessionLog log(dir.string());
    log.began("W1AW", T0);
    session::Message line;
    line.kind = session::Message::Kind::File;
    line.ours = false;
    line.from = "W1AW";
    line.text = "hello.sot, 3 KB: Saved";
    line.atMs = T0 + 5000;
    line.path = "/home/op/voice/hello.sot";
    line.voice = true;
    log.file("W1AW", line);
    log.ended("W1AW", T0 + 6000);
    std::vector<session::Message> rows = log.load("W1AW", 100);
    bool found = false;
    for (const session::Message& m : rows)
    {
        if (m.kind != session::Message::Kind::File) continue;
        found = true;
        CHECK(m.voice && !m.picture && m.path == "/home/op/voice/hello.sot");
    }
    CHECK(found);
}

// The Files tab's log keeps a voice message as voice.
void testFileLogKeepsVoice()
{
    files::FileHistory history;
    files::FinishedFile f;
    f.via = files::Via::Session;
    f.atMs = T0;
    f.outgoing = false;
    f.peer = "W1AW";
    f.name = "hello.sot";
    f.size = 3000;
    f.result = files::FinishedFile::Result::Saved;
    f.path = "/home/op/voice/hello.sot";
    f.voice = true;
    history.add(f);
    files::FinishedFile p = f;
    p.voice = false;
    p.picture = true;
    p.name = "shack.j2k";
    history.add(p);
    std::string text = files::saveFilesText(history, {});
    files::FileHistory back;
    std::vector<files::KeptPart> kept;
    CHECK(files::loadFilesText(text, back, kept));
    CHECK_EQ(back.entries().size(), (size_t)2);
    if (back.entries().size() == 2)
    {
        CHECK(back.entries()[0].voice && !back.entries()[0].picture);
        CHECK(back.entries()[1].picture && !back.entries()[1].voice);
    }
}

} // namespace

int main()
{
    testArrivalOrder();
    testPlayAndStop();
    testPushToTalkHolds();
    testClearAndForget();
    testBroadcastLogKeepsVoice();
    testSessionLogKeepsVoice();
    testFileLogKeepsVoice();
    return ragtime_test::finish();
}
