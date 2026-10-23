// SessionChat against a host played by hand: calling with its tries,
// the opening Hello both ways (the far end's arriving first), messages
// Sent and Delivered by the BUFFER count, Pause sending, Link quiet,
// Leave with its farewell, Leave now, the far end leaving, the link lost,
// no answer, a refused call, the data port closing, a stream that isn't
// Ragtime's, and the hook for what else rides in a session. Then the
// QRL and QRV broadcasts, the station list's session partner, and the
// log that carries a station's sessions on from one to the next. The
// clock is simulated.
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"
#include "ragtime/session/SessionChat.h"
#include "ragtime/session/SessionLog.h"

using namespace ragtime;
using link::HostLink;
using session::SessionChat;
using session::StreamAccount;
using Phase = SessionChat::Phase;
using Ending = SessionChat::Ending;
using MState = session::Message::State;

namespace
{

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC

std::vector<std::string> linesOf(const std::string& output)
{
    std::vector<std::string> lines;
    std::string line;
    for (char c : output)
    {
        if (c == '\r')
        {
            lines.push_back(line);
            line.clear();
        }
        else
        {
            line.push_back(c);
        }
    }
    return lines;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A station: HostLink through its opening, its SessionChat, and the far
// end's stream to play by hand.
struct Rig
{
    explicit Rig(uint64_t nowMs = T0) : link({"AG7EW", true, false}), chat(link), now(nowMs)
    {
        link.commandUp(now);
        say("OK\rOK\rOK\rOK\rBCAST PORT 2\rOK\r");
        link.takeCommandOutput();
        SessionChat::Settings s;
        s.callsign = "ag7ew";
        s.locator = "CN87";
        s.software = "Ragtime test";
        chat.setSettings(s);
        chat.dataPort(true, now);
        pump();
    }

    // The host's lines, then everything passed on as the app does.
    void say(const std::string& lines)
    {
        link.commandBytes(lines.data(), lines.size(), now);
        pump();
    }

    void pump()
    {
        for (const HostLink::Event& e : link.takeEvents()) chat.onEvent(e, now);
        std::vector<uint8_t> out = chat.takeDataOutput(now);
        written.insert(written.end(), out.begin(), out.end());
        for (const std::string& l : linesOf(link.takeCommandOutput())) commands.push_back(l);
    }

    void advance(uint64_t ms)
    {
        now += ms;
        link.tick(now);
        chat.tick(now);
        pump();
    }

    // What the far end writes on the data port.
    std::vector<protocol::Record> from(const std::vector<uint8_t>& bytes)
    {
        std::vector<protocol::Record> records = chat.dataBytes(bytes.data(), bytes.size(), now);
        pump();
        return records;
    }

    // Our stream as the far end reads it.
    std::vector<protocol::Record> ours()
    {
        protocol::StreamReader reader;
        std::vector<protocol::Record> records;
        reader.feed(written, records);
        return records;
    }

    bool sent(const std::string& command) const
    {
        for (const std::string& c : commands)
        {
            if (c == command) return true;
        }
        return false;
    }

    std::vector<SessionChat::Event> events()
    {
        return chat.takeEvents();
    }

    HostLink link;
    SessionChat chat;
    uint64_t now;
    std::vector<uint8_t> written;
    std::vector<std::string> commands;
};

// The far end of a session: its own numbering and stream.
struct Far
{
    explicit Far(const std::string& call) : numbering(call) {}

    std::vector<uint8_t> opening(uint64_t nowMs)
    {
        protocol::Hello hello;
        hello.acceptsSessions = true;
        hello.locator = "FN31pr";
        return protocol::streamOpening(numbering.record(hello, (uint32_t)(nowMs / 1000)));
    }

    std::vector<uint8_t> record(protocol::Body body, uint64_t nowMs)
    {
        return protocol::encodeRecord(numbering.record(std::move(body), (uint32_t)(nowMs / 1000)));
    }

    std::vector<uint8_t> text(const std::string& t, uint64_t nowMs)
    {
        protocol::Text body;
        body.text = t;
        return record(body, nowMs);
    }

    std::vector<uint8_t> farewell(uint64_t nowMs)
    {
        protocol::Status qrv;
        qrv.state = protocol::Status::State::Qrv;
        return record(qrv, nowMs);
    }

    protocol::Numbering numbering;
};

// A rig in a session with W1AW, both Hellos in.
void connect(Rig& rig, Far& far)
{
    CHECK(rig.chat.call("W1AW", rig.now));
    rig.pump();
    rig.say("OK\rCONNECTED AG7EW W1AW 2300\r");
    rig.from(far.opening(rig.now));
}

//-------------------------------------------------------------------------

void testTheStreamAccount()
{
    StreamAccount a;
    a.wrote(100, 0);
    // A count from before the host read the write settles nothing.
    a.buffer(0, 10);
    CHECK_EQ(a.acknowledged(), (uint64_t)0);
    // The host reads it: the count rises by what it read.
    a.buffer(100, 20);
    CHECK_EQ(a.read(), (uint64_t)100);
    CHECK_EQ(a.acknowledged(), (uint64_t)0);
    a.buffer(40, 3000);
    CHECK_EQ(a.acknowledged(), (uint64_t)60);
    a.buffer(0, 4000);
    CHECK_EQ(a.acknowledged(), (uint64_t)100);

    // A read that takes a bigger acknowledgement in the same step lowers
    // the count; a 0 well after the write settles it all the same.
    a.wrote(100, 4100);
    a.buffer(100, 4200);
    a.buffer(40, 4300);
    CHECK_EQ(a.acknowledged(), (uint64_t)160);
    a.wrote(50, 5000);
    a.buffer(30, 5001);   // 40 + 50 read - 60 acknowledged: no rise, so the 50 aren't known read
    CHECK_EQ(a.read(), (uint64_t)200);
    CHECK_EQ(a.acknowledged(), (uint64_t)170);
    a.buffer(0, 5000 + StreamAccount::SETTLE_MS - 1);
    CHECK_EQ(a.acknowledged(), (uint64_t)200);
    a.buffer(0, 5000 + StreamAccount::SETTLE_MS);
    CHECK_EQ(a.acknowledged(), (uint64_t)250);
    CHECK_EQ(a.waiting(), 0);

    // Never more than was written.
    a.buffer(500, 9000);
    CHECK_EQ(a.read(), (uint64_t)250);
    a.reset();
    CHECK_EQ(a.written(), (uint64_t)0);
}

void testCallingAndTheOpening()
{
    Rig rig;
    CHECK(rig.chat.phase() == Phase::None);
    CHECK(!rig.chat.call("AG7EW", rig.now)); // not ourselves
    CHECK(rig.chat.call("w1aw", rig.now));
    rig.pump();
    CHECK(rig.sent("CONNECT AG7EW W1AW"));
    CHECK(rig.chat.phase() == Phase::Calling && rig.chat.active());
    CHECK_EQ(rig.chat.strip(rig.now).text, std::string("Calling W1AW, try 1 of 5"));
    CHECK(!rig.chat.call("K1ABC", rig.now)); // one at a time
    std::vector<SessionChat::Event> events = rig.events();
    CHECK(events.size() == 1 && events[0].type == SessionChat::Event::Type::Calling && events[0].peer == "W1AW");

    // Each call keys the transmitter.
    rig.say("OK\rPTT ON\rPTT OFF\rPTT ON\r");
    CHECK_EQ(rig.chat.callTry(), 2);
    CHECK_EQ(rig.chat.strip(rig.now).text, std::string("Calling W1AW, try 2 of 5"));
    rig.say("PTT OFF\r");

    // The far end's Hello comes before CONNECTED does: it waits for it.
    Far far("W1AW");
    CHECK(rig.from(far.opening(rig.now)).empty());
    CHECK(rig.written.empty());
    rig.advance(1000);
    rig.say("CONNECTED AG7EW W1AW 2300\r");
    CHECK(rig.chat.phase() == Phase::Connected);
    CHECK(!rig.chat.incoming());
    CHECK(rig.chat.greeted());
    CHECK_EQ(rig.chat.theirHello().locator, std::string("FN31pr"));
    CHECK_EQ(rig.chat.bandwidthHz(), 2300);
    events = rig.events();
    CHECK(events.size() == 1 && events[0].type == SessionChat::Event::Type::Started && !events[0].incoming);

    // Our opening: 0xA0, then our Hello, numbered afresh.
    CHECK(!rig.written.empty() && rig.written[0] == protocol::FRAME_BYTE);
    std::vector<protocol::Record> ours = rig.ours();
    CHECK(ours.size() == 1 && ours[0].as<protocol::Hello>() != nullptr);
    if (!ours.empty())
    {
        CHECK_EQ(ours[0].header.callsign, std::string("AG7EW"));
        CHECK_EQ(ours[0].header.seq, (uint32_t)0);
        CHECK_EQ(ours[0].as<protocol::Hello>()->locator, std::string("CN87"));
        CHECK_EQ(ours[0].as<protocol::Hello>()->software, std::string("Ragtime test"));
    }
    std::string strip = rig.chat.strip(rig.now).text;
    CHECK(contains(strip, "Connected to W1AW"));
    CHECK(contains(strip, "2300 Hz"));
    CHECK(contains(strip, "heard just now"));
}

void testMessagesDeliveredByTheBufferCount()
{
    Rig rig;
    Far far("W1AW");
    connect(rig, far);
    CHECK_EQ(rig.chat.send("   ", rig.now), (uint64_t)0);
    CHECK_EQ(rig.chat.send(std::string(SessionChat::MAX_TYPED + 1, 'x'), rig.now), (uint64_t)0);

    uint64_t id = rig.chat.send("Good evening Hiram, how copy?", rig.now);
    CHECK(id != 0);
    rig.pump();
    CHECK(rig.chat.find(id)->state == MState::Sent);
    std::vector<protocol::Record> ours = rig.ours();
    CHECK(ours.size() == 2);
    if (ours.size() == 2)
    {
        CHECK(ours[1].as<protocol::Text>() && ours[1].as<protocol::Text>()->text == "Good evening Hiram, how copy?");
        CHECK_EQ(ours[1].header.seq, (uint32_t)1); // the session's first item
    }

    // The host reads all we wrote, then the far end acknowledges it.
    rig.say("BUFFER " + std::to_string(rig.written.size()) + "\r");
    CHECK(rig.chat.find(id)->state == MState::Sent);
    CHECK(contains(rig.chat.strip(rig.now).text, std::to_string(rig.written.size()) + " bytes waiting"));
    rig.advance(5000);
    rig.say("BUFFER 0\r");
    CHECK(rig.chat.find(id)->state == MState::Delivered);
    CHECK_EQ(std::string(session::stateLabel(MState::Delivered)), std::string("Delivered"));

    // Theirs.
    rig.advance(7000);
    rig.from(far.text("Fine business, 73", rig.now));
    CHECK_EQ(rig.chat.lastHeardMs(), rig.now);
    const session::Message& last = rig.chat.messages().back();
    CHECK(!last.ours && last.from == "W1AW" && last.text == "Fine business, 73");
    bool heard = false;
    for (const auto& e : rig.events()) heard = heard || e.type == SessionChat::Event::Type::Heard;
    CHECK(heard);

    // Pause sending holds a message as Queued until it is let go.
    rig.chat.setPaused(true, rig.now);
    uint64_t held = rig.chat.send("wait for it", rig.now);
    size_t before = rig.written.size();
    rig.pump();
    CHECK(rig.chat.find(held)->state == MState::Queued && rig.written.size() == before);
    rig.chat.setPaused(false, rig.now);
    rig.pump();
    CHECK(rig.chat.find(held)->state == MState::Sent && rig.written.size() > before);

    // Re-send: the text again as a new message, with a number of its own;
    // not while one is still Queued.
    uint64_t again = rig.chat.resend(id, rig.now);
    CHECK(again != 0 && again != id);
    rig.pump();
    CHECK(rig.chat.find(again)->state == MState::Sent && rig.chat.find(again)->text == rig.chat.find(id)->text);
    CHECK(rig.chat.find(again)->seq > rig.chat.find(held)->seq);
    rig.chat.setPaused(true, rig.now);
    uint64_t waiting = rig.chat.send("still queued", rig.now);
    CHECK_EQ(rig.chat.resend(waiting, rig.now), (uint64_t)0);
    CHECK_EQ(rig.chat.resend(12345, rig.now), (uint64_t)0);
    rig.chat.setPaused(false, rig.now);
    rig.pump();

    // The session ends before it is acknowledged.
    rig.say("DISCONNECTED\r");
    CHECK(rig.chat.find(held)->state == MState::NotDelivered);
    CHECK(rig.chat.find(id)->state == MState::Delivered);
    CHECK(!rig.chat.active());
    CHECK_EQ(rig.chat.send("too late", rig.now), (uint64_t)0);
    CHECK_EQ(rig.chat.resend(held, rig.now), (uint64_t)0);     // no session up
}

void testLinkQuiet()
{
    Rig rig;
    Far far("W1AW");
    connect(rig, far);
    // Nothing waiting: a session where nobody types is not quiet.
    rig.advance(5 * 60 * 1000);
    CHECK(!rig.chat.quiet(rig.now));
    CHECK(contains(rig.chat.strip(rig.now).text, "heard 5 min ago"));

    rig.chat.send("are you there?", rig.now);
    rig.pump();
    rig.say("BUFFER " + std::to_string(rig.written.size()) + "\r");
    rig.advance(SessionChat::QUIET_MS - 1000);
    CHECK(!rig.chat.quiet(rig.now));
    rig.advance(40 * 1000);
    CHECK(rig.chat.quiet(rig.now));
    SessionChat::Strip strip = rig.chat.strip(rig.now);
    CHECK(strip.amber);
    CHECK(contains(strip.text, "Link quiet \xc2\xb7 1:09 \xc2\xb7 data2g-host is still retrying"));
    CHECK(rig.chat.phase() == Phase::Connected); // quiet is not closed

    // Something back: no longer quiet.
    rig.say("BUFFER 3\r");
    CHECK(!rig.chat.quiet(rig.now));
    CHECK(!rig.chat.strip(rig.now).amber);
}

void testLeaveWithAFarewell()
{
    Rig rig;
    Far far("W1AW");
    connect(rig, far);
    rig.advance(14 * 60 * 1000);
    size_t before = rig.written.size();
    CHECK(rig.chat.leave(rig.now));
    rig.pump();
    CHECK(rig.chat.phase() == Phase::Leaving);
    // The farewell first, and DISCONNECT only once the host has read it.
    std::vector<protocol::Record> ours = rig.ours();
    CHECK(!ours.empty() && ours.back().as<protocol::Status>() != nullptr
          && ours.back().as<protocol::Status>()->state == protocol::Status::State::Qrv);
    CHECK(rig.written.size() > before);
    CHECK(!rig.sent("DISCONNECT"));
    rig.say("BUFFER " + std::to_string(rig.written.size()) + "\r");
    CHECK(rig.sent("DISCONNECT"));
    CHECK(contains(rig.chat.strip(rig.now).text, "Leaving, waiting for"));
    CHECK(!rig.chat.leave(rig.now)); // already leaving
    rig.say("OK\rBUFFER 0\rDISCONNECTED\r");
    CHECK(rig.chat.phase() == Phase::Ended && rig.chat.ending() == Ending::Left);
    CHECK_EQ(rig.chat.strip(rig.now).text, std::string("You left the session with W1AW at 00:14, after 14 min"));
    CHECK(!rig.chat.strip(rig.now).live);

    // A host that never reads the farewell: DISCONNECT after the wait.
    Rig slow;
    Far far2("W1AW");
    connect(slow, far2);
    slow.chat.leave(slow.now);
    slow.pump();
    slow.advance(SessionChat::FAREWELL_WAIT_MS - 1);
    CHECK(!slow.sent("DISCONNECT"));
    slow.advance(1);
    CHECK(slow.sent("DISCONNECT"));

    // Leave now while the host waits.
    CHECK(slow.chat.leaveNow(slow.now));
    slow.pump();
    CHECK(slow.sent("ABORT"));
    // A host that never says DISCONNECTED: the session ends here anyway.
    slow.advance(HostLink::ABORT_WAIT_MS);
    CHECK(slow.chat.phase() == Phase::Ended && slow.chat.ending() == Ending::Left);
}

void testTheFarEndLeavesOrIsLost()
{
    // Its farewell, then the host's DISCONNECTED.
    Rig rig;
    Far far("W1AW");
    connect(rig, far);
    rig.advance(32 * 60 * 1000);
    rig.from(far.farewell(rig.now));
    rig.say("DISCONNECTED\r");
    CHECK(rig.chat.ending() == Ending::LeftThere);
    CHECK_EQ(rig.chat.strip(rig.now).text, std::string("Session ended by W1AW at 00:32"));
    // Shown in local time, Pacific daylight time here.
    CHECK_EQ(rig.chat.strip(rig.now, -7 * 3600).text, std::string("Session ended by W1AW at 17:32"));

    // The farewell read after DISCONNECTED, the ports racing: still a leaving.
    Rig racing;
    Far far3("W1AW");
    connect(racing, far3);
    racing.say("DISCONNECTED\r");
    CHECK(racing.chat.ending() == Ending::LinkLost);
    racing.advance(SessionChat::AFTERGLOW_MS / 2);
    racing.from(far3.farewell(racing.now));
    CHECK(racing.chat.ending() == Ending::LeftThere);
    // Too late, and it is the next session's business.
    Rig late;
    Far far4("W1AW");
    connect(late, far4);
    late.say("DISCONNECTED\r");
    late.advance(SessionChat::AFTERGLOW_MS);
    late.from(far4.farewell(late.now));
    CHECK(late.chat.ending() == Ending::LinkLost);

    // No farewell, after three minutes of silence.
    Rig lost;
    Far far2("W1AW");
    connect(lost, far2);
    lost.advance(3 * 60 * 1000);
    lost.say("DISCONNECTED\r");
    CHECK(lost.chat.ending() == Ending::LinkLost);
    CHECK_EQ(lost.chat.strip(lost.now).text, std::string("Link lost at 00:03 after 3 min without contact"));

    // The next session starts clean, though the last one's stream ended
    // mid-record.
    std::vector<uint8_t> half = far2.text("this record breaks off", lost.now);
    half.resize(half.size() / 2);
    lost.from(half);
    Far again("W1AW");
    lost.say("CONNECTED W1AW AG7EW\r");
    CHECK(lost.chat.incoming());
    lost.from(again.opening(lost.now));
    CHECK(lost.chat.greeted() && !lost.chat.foreign());
    CHECK(contains(lost.chat.strip(lost.now).text, "W1AW started a session with you"));
    CHECK(lost.chat.messages().empty());
}

void testCallsThatDontConnect()
{
    Rig rig;
    CHECK(rig.chat.call("K1ABC", rig.now));
    rig.say("OK\rDISCONNECTED\r");
    CHECK(rig.chat.ending() == Ending::NoAnswer);
    CHECK_EQ(rig.chat.strip(rig.now).text, std::string("K1ABC didn't answer (00:00)"));

    CHECK(rig.chat.call("K1ABC", rig.now));
    rig.say("WRONG\r");
    CHECK(rig.chat.ending() == Ending::Refused);

    // Cancel: Leave while calling.
    CHECK(rig.chat.call("W1AW", rig.now));
    rig.pump();
    CHECK(rig.chat.leave(rig.now));
    rig.pump();
    CHECK(rig.sent("ABORT"));
    CHECK(contains(rig.chat.strip(rig.now).text, "Stopping the call to W1AW"));
    rig.say("OK\rOK\rDISCONNECTED\r");
    CHECK(rig.chat.ending() == Ending::Cancelled);

    // A host that never settles a call is given up on.
    Rig mute;
    CHECK(mute.chat.call("W1AW", mute.now));
    mute.pump();
    mute.advance(SessionChat::CALL_GIVE_UP_MS);
    CHECK(mute.sent("ABORT"));
    mute.say("OK\rOK\rDISCONNECTED\r");
    CHECK(mute.chat.ending() == Ending::NoAnswer);

    // No data port, no call.
    Rig noData;
    noData.chat.dataPort(false, noData.now);
    CHECK(!noData.chat.call("W1AW", noData.now));
}

void testTheDataPortCloses()
{
    Rig rig;
    Far far("W1AW");
    connect(rig, far);
    rig.chat.dataPort(false, rig.now);
    rig.pump();
    CHECK(rig.sent("ABORT"));
    rig.say("OK\rDISCONNECTED\r");
    CHECK(rig.chat.ending() == Ending::DataLost);
    CHECK(contains(rig.chat.strip(rig.now).text, "The session data port closed"));
}

void testAStreamThatIsntRagtimes()
{
    Rig rig;
    CHECK(rig.chat.call("W1AW", rig.now));
    rig.say("OK\rCONNECTED AG7EW W1AW\r");
    std::string vara = "Hello from some other program";
    rig.from(std::vector<uint8_t>(vara.begin(), vara.end()));
    CHECK(rig.chat.foreign() && !rig.chat.greeted());
    CHECK(contains(rig.chat.strip(rig.now).text, "W1AW's end isn't speaking Ragtime"));
    CHECK(rig.chat.phase() == Phase::Connected); // the operator decides to leave
}

// What package G's files will plug in as.
struct Traffic : session::SessionTraffic
{
    void greeted(const std::string& peer, const protocol::Hello& hello) override
    {
        greetings.push_back(peer + " " + std::to_string(hello.takes));
    }
    void onRecord(const std::string&, const protocol::Record& record, uint64_t) override
    {
        records.push_back(record.type());
    }
    void arriving(const std::string&, uint32_t offer, size_t bytes) override
    {
        arrivingOffer = offer;
        arrivingBytes = bytes;
    }
    void write(const std::string&, protocol::Numbering& numbering, uint64_t streamOffset, uint64_t nowMs,
               std::vector<uint8_t>& out) override
    {
        if (!offer) return;
        offer = false;
        offset = streamOffset;
        protocol::FileOffer o;
        o.size = 7000;
        o.name = "FRED.TXT";
        protocol::encodeRecord(numbering.record(o, (uint32_t)(nowMs / 1000)), out);
    }
    void acknowledged(const std::string&, uint64_t streamAcked) override { acked = streamAcked; }
    void leaving(const std::string&, protocol::Numbering& numbering, uint64_t nowMs,
                 std::vector<uint8_t>& out) override
    {
        protocol::FileCancel c;
        c.offer = 1;
        c.reason = (uint8_t)protocol::CancelReason::SenderStopped;
        protocol::encodeRecord(numbering.record(c, (uint32_t)(nowMs / 1000)), out);
    }
    void ended(const std::string& peer, uint64_t) override { endedWith = peer; }
    bool busy(const std::string&) const override { return isBusy; }

    std::vector<std::string> greetings;
    std::vector<uint8_t> records;
    uint32_t arrivingOffer = 0;
    size_t arrivingBytes = 0;
    bool offer = true;
    uint64_t offset = 0;
    uint64_t acked = 0;
    std::string endedWith;
    bool isBusy = false;
};

void testWhatElseRidesInASession()
{
    Rig rig;
    Traffic traffic;
    rig.chat.setTraffic(&traffic);
    Far far("W1AW");
    connect(rig, far);
    CHECK(traffic.greetings.size() == 1 && traffic.greetings[0] == "W1AW 31");

    // It writes behind our Hello, and is told where.
    std::vector<protocol::Record> ours = rig.ours();
    CHECK(ours.size() == 2 && ours[1].as<protocol::FileOffer>() != nullptr);
    CHECK_EQ(traffic.offset, (uint64_t)protocol::streamOpening(ours[0]).size());
    if (ours.size() == 2) CHECK_EQ(ours[1].header.seq, (uint32_t)1);

    // Records that aren't the chat's go to it, a FileData still arriving
    // included.
    protocol::FileAnswer answer;
    answer.offer = 1;
    answer.accept = true;
    rig.from(far.record(answer, rig.now));
    protocol::FileData data;
    data.offset = 0;
    data.bytes.assign(1000, 0x55);
    std::vector<uint8_t> piece = protocol::encodeRecord(far.numbering.record(data, (uint32_t)(rig.now / 1000), 1));
    rig.from(std::vector<uint8_t>(piece.begin(), piece.begin() + 600));
    CHECK(traffic.records.size() == 1 && traffic.records[0] == (uint8_t)protocol::RecordType::FileAnswer);
    CHECK_EQ(traffic.arrivingOffer, (uint32_t)1);
    CHECK(traffic.arrivingBytes > 0 && traffic.arrivingBytes < 1000);
    rig.from(std::vector<uint8_t>(piece.begin() + 600, piece.end()));
    CHECK(traffic.records.size() == 2 && traffic.records[1] == (uint8_t)protocol::RecordType::FileData);

    // Acknowledgements reach it.
    rig.say("BUFFER " + std::to_string(rig.written.size()) + "\r");
    rig.advance(3000);
    rig.say("BUFFER 0\r");
    CHECK_EQ(traffic.acked, (uint64_t)rig.written.size());

    // Leave asks first while it is busy; its last word goes before the
    // farewell.
    traffic.isBusy = true;
    CHECK(rig.chat.trafficBusy());
    rig.chat.leave(rig.now);
    rig.pump();
    ours = rig.ours();
    CHECK(ours.size() >= 2 && ours[ours.size() - 2].as<protocol::FileCancel>() != nullptr
          && ours.back().as<protocol::Status>() != nullptr);
    rig.say("DISCONNECTED\r");
    CHECK_EQ(traffic.endedWith, std::string("W1AW"));
}

void testTheHostLinkSide()
{
    // A CONNECT the host answers WRONG ends the call.
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    std::string open = "OK\rOK\rOK\rOK\rBCAST PORT 2\rOK\r";
    link.commandBytes(open.data(), open.size(), 0);
    link.takeEvents();
    CHECK(link.call("W1AW", 10));
    std::string wrong = "WRONG\r";
    link.commandBytes(wrong.data(), wrong.size(), 20);
    CHECK(link.sessionState() == HostLink::SessionState::None);
    bool refused = false;
    for (const auto& e : link.takeEvents())
        refused = refused || (e.type == HostLink::Event::Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::Refused);
    CHECK(refused);

    // Whether a broadcast of ours is with the host.
    CHECK(!link.burstWithHost());
    link.broadcast({{0xA0, 1}}, "", 30);
    CHECK(link.burstWithHost());
}

void testQrlAndQrv()
{
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    std::string open = "OK\rOK\rOK\rOK\rBCAST PORT 2\rOK\r";
    link.commandBytes(open.data(), open.size(), 0);
    link.takeKissOutput();
    broadcast::BroadcastChat chat(link);
    broadcast::BroadcastChat::Settings s;
    s.callsign = "AG7EW";
    chat.setSettings(s, 0);

    auto statusOut = [&](protocol::Status::State& state, std::string& partner) {
        std::vector<uint8_t> out = link.takeKissOutput();
        link::KissDecoder decoder;
        std::vector<link::KissFrame> frames;
        decoder.feed(out.data(), out.size(), frames);
        for (const link::KissFrame& f : frames)
        {
            std::vector<protocol::Record> records;
            protocol::decodeFrame(f.payload.data() + 2, f.payload.size() - 2, records);
            for (const auto& r : records)
            {
                if (const auto* st = r.as<protocol::Status>())
                {
                    state = st->state;
                    partner = st->partner;
                    // Acknowledge it, so the next burst can go.
                    std::vector<uint8_t> ack = link::kissEncodeAckMode(2, (uint16_t)((f.payload[0] << 8) | f.payload[1]), {});
                    link.kissBytes(ack.data(), ack.size(), 0);
                    return true;
                }
            }
        }
        return false;
    };

    // No QRV without a QRL.
    CHECK(!chat.announce(protocol::Status::State::Qrv, "", "", 0));
    CHECK(chat.announce(protocol::Status::State::Qrl, "w1aw", "", 0));
    protocol::Status::State state = protocol::Status::State::Qrt;
    std::string partner;
    CHECK(statusOut(state, partner));
    CHECK(state == protocol::Status::State::Qrl && partner == "W1AW");
    CHECK(chat.announce(protocol::Status::State::Qrv, "", "", 10));
    CHECK(statusOut(state, partner));
    CHECK(state == protocol::Status::State::Qrv && partner.empty());
    CHECK(!chat.announce(protocol::Status::State::Qrv, "", "", 20)); // once

    // A QRL still queued when the session ends is taken back, with no QRV.
    link.setPaused(true, 30);
    CHECK(chat.announce(protocol::Status::State::Qrl, "W1AW", "", 30));
    CHECK(!chat.announce(protocol::Status::State::Qrv, "", "", 40));
    link.setPaused(false, 50);
    CHECK(link.takeKissOutput().empty());
}

void testTheSessionPartnerOnTheStationList()
{
    broadcast::Stations stations;
    stations.setMyCallsign("AG7EW");
    protocol::Record hello;
    hello.header = {"KC2G", 0, 0};
    protocol::Hello takesSessions;
    takesSessions.acceptsSessions = true;
    hello.body = takesSessions;
    stations.onRecord(hello, T0);
    hello.header.callsign = "G4XYZ";
    stations.onRecord(hello, T0 + 1000);

    // A caller never heard on the group is listed at once, at the top.
    stations.setPartner("vk3abc", true, T0 + 2000);
    const broadcast::Station* s = stations.find("VK3ABC");
    CHECK(s != nullptr);
    if (s != nullptr) CHECK_EQ(stations.status(*s, T0 + 2000).word, std::string("QSO w/ AG7EW"));
    for (broadcast::Column c : {broadcast::Column::Heard, broadcast::Column::Call, broadcast::Column::Status})
    {
        for (bool up : {true, false})
        {
            std::vector<const broadcast::Station*> order = stations.sorted(c, up, T0 + 2000);
            CHECK(!order.empty() && order[0]->call == "VK3ABC");
        }
    }

    // Calling shows as Calling; none, and it's an ordinary row again.
    stations.setPartner("KC2G", false, T0 + 3000);
    CHECK_EQ(stations.status(*stations.find("KC2G"), T0 + 3000).word, std::string("Calling"));
    CHECK_EQ(stations.sorted(broadcast::Column::Heard, true, T0 + 3000)[0]->call, std::string("KC2G"));
    stations.setPartner("", false, T0 + 4000);
    CHECK_EQ(stations.status(*stations.find("KC2G"), T0 + 4000).word, std::string("QRV"));
    CHECK(stations.partner().empty());
}

void testTheLogCarriesOn()
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-session-history-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    session::SessionLog log(dir.string());
    using Kind = session::Message::Kind;

    // A first session with W1AW: one of ours delivered, one of theirs, and
    // one of ours the session ends under.
    {
        Rig rig;
        rig.chat.setLog(&log);
        Far far("W1AW");
        connect(rig, far);
        uint64_t id = rig.chat.send("Good evening Hiram", rig.now);
        rig.pump();
        rig.say("BUFFER " + std::to_string(rig.written.size()) + "\r");
        rig.advance(5000);
        rig.say("BUFFER 0\r");
        CHECK(rig.chat.find(id)->state == MState::Delivered);
        rig.from(far.text("Fine business, how copy?", rig.now));
        rig.chat.send("73", rig.now);
        rig.pump();
        rig.advance(60 * 1000);
        rig.from(far.farewell(rig.now));
        rig.say("DISCONNECTED\r");
        CHECK(rig.chat.ending() == Ending::LeftThere);
        // This session's own rows aren't past; no rule while it is shown.
        CHECK_EQ(rig.chat.messages().size(), (size_t)3);
        CHECK(!rig.chat.messages().front().past);
    }

    // After a restart: calling W1AW brings it all back, closed by a rule,
    // and the new session carries on below it.
    Rig rig(T0 + 3600 * 1000);
    rig.chat.setLog(&log);
    CHECK(rig.chat.messages().empty());
    Far far("W1AW");
    connect(rig, far);
    const auto& rows = rig.chat.messages();
    CHECK_EQ(rows.size(), (size_t)4);
    if (rows.size() == 4)
    {
        CHECK(rows[0].past && rows[0].ours && rows[0].text == "Good evening Hiram" && rows[0].state == MState::Delivered);
        CHECK(rows[1].past && !rows[1].ours && rows[1].text == "Fine business, how copy?");
        CHECK(rows[2].text == "73" && rows[2].state == MState::NotDelivered);
        CHECK(rows[3].kind == Kind::Rule && rows[3].beganMs == T0 && rows[3].atMs == T0 + 65 * 1000);
        CHECK(rows[0].id != 0 && rows[1].id != rows[0].id);
        // An earlier message of ours can go again in this session.
        CHECK(SessionChat::canResend(rows[0]) && !SessionChat::canResend(rows[3]));
    }
    uint64_t again = rig.chat.resend(rows.front().id, rig.now);
    CHECK(again != 0);
    rig.pump();
    CHECK(!rig.chat.find(again)->past && rig.chat.find(again)->text == "Good evening Hiram");
    CHECK_EQ(rig.chat.messages().size(), (size_t)5);
    // Its link lost, the far end's last words still come in the afterglow,
    // and go in the log before the session's end.
    rig.say("DISCONNECTED\r");
    rig.from(far.text("last words", rig.now));
    rig.advance(SessionChat::AFTERGLOW_MS);
    CHECK_EQ(rig.chat.messages().back().text, std::string("last words"));

    // Another station has its own log: nothing yet.
    rig.from(far.opening(rig.now));     // none of this session's business
    CHECK(rig.chat.call("K1ABC", rig.now));
    rig.pump();
    CHECK(rig.chat.messages().empty());
    rig.say("OK\rDISCONNECTED\r");
    CHECK(rig.chat.ending() == Ending::NoAnswer);

    // An incoming session from W1AW: both earlier sessions, a rule each.
    Far back("W1AW");
    rig.say("CONNECTED W1AW AG7EW\r");
    rig.from(back.opening(rig.now));
    CHECK(rig.chat.incoming());
    const auto& all = rig.chat.messages();
    CHECK_EQ(all.size(), (size_t)7);
    if (all.size() == 7)
    {
        CHECK(all[3].kind == Kind::Rule);
        CHECK(all[4].text == "Good evening Hiram" && all[4].state == MState::NotDelivered);
        CHECK_EQ(all[5].text, std::string("last words"));
        // Ended when the host said so, not when the afterglow was over.
        CHECK(all[6].kind == Kind::Rule && all[6].beganMs == T0 + 3600 * 1000);
        CHECK_EQ(all[6].atMs, T0 + 3600 * 1000);
    }
    // The call that never connected left no session in the log.
    CHECK(log.load("K1ABC", 100).empty());

    // No log: each session starts empty, as before.
    Rig plain;
    Far far2("W1AW");
    connect(plain, far2);
    CHECK(plain.chat.messages().empty());

    std::filesystem::remove_all(dir, ec);
}

void testTheLengths()
{
    CHECK_EQ(session::sessionLength(30 * 1000), std::string("less than a minute"));
    CHECK_EQ(session::sessionLength(14 * 60 * 1000), std::string("14 min"));
    CHECK_EQ(session::sessionLength(65 * 60 * 1000), std::string("1 h 5 min"));
    CHECK_EQ(session::sessionLength(120 * 60 * 1000), std::string("2 h"));
    CHECK_EQ(session::clockLength(70 * 1000), std::string("1:10"));
    CHECK_EQ(session::clockLength(3723 * 1000), std::string("1:02:03"));
    CHECK_EQ(session::clockTime(T0 + (14 * 60 + 32) * 60 * 1000, 0), std::string("14:32"));
    CHECK_EQ(session::clockTime(T0, -7 * 3600), std::string("17:00"));
}

} // namespace

int main()
{
    testTheStreamAccount();
    testCallingAndTheOpening();
    testMessagesDeliveredByTheBufferCount();
    testLinkQuiet();
    testLeaveWithAFarewell();
    testTheFarEndLeavesOrIsLost();
    testCallsThatDontConnect();
    testTheDataPortCloses();
    testAStreamThatIsntRagtimes();
    testWhatElseRidesInASession();
    testTheHostLinkSide();
    testQrlAndQrv();
    testTheSessionPartnerOnTheStationList();
    testTheLengths();
    testTheLogCarriesOn();
    return ragtime_test::finish();
}
