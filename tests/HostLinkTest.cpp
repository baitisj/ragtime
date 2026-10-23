// HostLink against a host played by hand: the opening, the group, bursts
// with their ACKMODE tags and outcomes, a session holding the group, and
// sessions called, answered, left and lost. The clock is simulated.
#include <cstdint>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/HostLink.h"

using namespace ragtime::link;
using Type = HostLink::Event::Type;
using BurstState = HostLink::BurstState;

namespace
{

// The command lines written, without their CRs.
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

void say(HostLink& link, const std::string& lines, uint64_t nowMs)
{
    link.commandBytes(lines.data(), lines.size(), nowMs);
}

bool hasEvent(const std::vector<HostLink::Event>& events, Type type)
{
    for (const auto& e : events)
    {
        if (e.type == type) return true;
    }
    return false;
}

// The tags of the ACKMODE frames written to the KISS port.
std::vector<uint16_t> tagsWritten(HostLink& link, int port, std::vector<std::vector<uint8_t>>* frames = nullptr)
{
    std::vector<uint8_t> out = link.takeKissOutput();
    KissDecoder decoder;
    std::vector<KissFrame> kiss;
    decoder.feed(out.data(), out.size(), kiss);
    std::vector<uint16_t> tags;
    for (const KissFrame& f : kiss)
    {
        CHECK(f.command == KISS_ACKMODE && f.port == port && f.payload.size() >= 2);
        if (f.payload.size() < 2) continue;
        tags.push_back((uint16_t)((f.payload[0] << 8) | f.payload[1]));
        if (frames != nullptr) frames->emplace_back(f.payload.begin() + 2, f.payload.end());
    }
    return tags;
}

void ackTags(HostLink& link, int port, const std::vector<uint16_t>& tags, uint64_t nowMs)
{
    std::vector<uint8_t> in;
    for (uint16_t tag : tags)
    {
        std::vector<uint8_t> ack = kissEncodeAckMode(port, tag, {});
        in.insert(in.end(), ack.begin(), ack.end());
    }
    link.kissBytes(in.data(), in.size(), nowMs);
}

// A link through its opening, with the group open on port 2 and the
// known modes listed.
HostLink opened(uint64_t nowMs, bool narrow = false)
{
    HostLink link({"ag7ew", true, narrow});
    link.commandUp(nowMs);
    link.takeCommandOutput();
    std::string answers = "OK\rOK\rOK\rOK\rBCAST PORT 2\r";
    for (const ModeInfo& m : knownModes())
    {
        answers += "MODE " + m.name + " " + std::to_string(m.bandwidthHz) + " " + std::to_string(m.bytesPerCodeword) +
                   " " + std::to_string(m.maxCodewords) + " 1.0 10.0\r";
    }
    answers += "OK\r";
    say(link, answers, nowMs);
    link.takeEvents();
    return link;
}

void testTheOpening()
{
    HostLink link({"ag7ew", true, false});
    CHECK(!link.commandConnected());
    link.commandUp(1000);
    std::vector<std::string> lines = linesOf(link.takeCommandOutput());
    const std::vector<std::string> expected = {"MYCALL AG7EW", "CHAT ON", "LISTEN ON", "BW2300",
                                               "BCAST OPEN D2G FROM AG7EW", "MODES"};
    CHECK(lines == expected);
    CHECK(!link.groupOpen());

    say(link, "OK\rOK\rOK\rOK\rBCAST PORT 2\rMODE fsk16r25-r1/3 500 38 9 4.98 89.22\r", 1100);
    CHECK(link.groupOpen());
    CHECK_EQ(link.groupPort(), 2);
    // The list counts once its OK comes.
    CHECK(link.modes().empty());
    say(link, "MODE qpsk-r1/3 1200 76 64 1.78 74.36\rOK\r", 1200);
    CHECK_EQ(link.modes().size(), (size_t)2);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::GroupOpen));
    CHECK(hasEvent(events, Type::ModesChanged));

    // Not taking sessions, and the 500 Hz button.
    HostLink narrow({"W1AW", false, true});
    narrow.commandUp(0);
    lines = linesOf(narrow.takeCommandOutput());
    CHECK(lines.size() == 6 && lines[2] == "LISTEN OFF" && lines[3] == "BW500");

    // Changed later: the host is told, and the modes asked for again.
    link.setNarrow(true, 2000);
    link.setAcceptSessions(false, 2000);
    lines = linesOf(link.takeCommandOutput());
    CHECK(lines == std::vector<std::string>({"BW500", "MODES", "LISTEN OFF"}));
    link.setNarrow(true, 2000);
    CHECK(link.takeCommandOutput().empty());
}

void testReports()
{
    HostLink link = opened(0);
    say(link, "PTT ON\rBUSY ON\rMODE qpsk-r1/3\rBCAST 2 HEARD W1AW\rBCAST 2 LOST 3\rIAMALIVE\r", 10);
    CHECK(link.ptt() && link.busy());
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::Ptt) && hasEvent(events, Type::Busy) && hasEvent(events, Type::ModeUsed) &&
          hasEvent(events, Type::HeardFrom) && hasEvent(events, Type::GroupLost));
    // The same again is no news.
    say(link, "PTT ON\r", 20);
    CHECK(!hasEvent(link.takeEvents(), Type::Ptt));

    // Frames heard on the group port come up; other ports' don't.
    std::vector<uint8_t> in = kissEncode(2, {0xA0, 1, 2});
    std::vector<uint8_t> other = kissEncode(5, {0xA0, 9});
    in.insert(in.end(), other.begin(), other.end());
    link.kissBytes(in.data(), in.size(), 30);
    events = link.takeEvents();
    int heard = 0;
    for (const auto& e : events)
    {
        if (e.type == Type::Heard)
        {
            heard++;
            CHECK(e.frame == std::vector<uint8_t>({0xA0, 1, 2}));
        }
    }
    CHECK_EQ(heard, 1);
}

void testSnrReports()
{
    HostLink link = opened(0);
    // A host since Data2G PR #98 ends HEARD and MISSED with an SNR.
    say(link, "BCAST 2 HEARD W1AW 12.3\rBCAST * MISSED qpsk-r1/3 4 -2.5\rBCAST 2 HEARD K1ABC\r", 10);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK_EQ(events.size(), (size_t)3);
    if (events.size() == 3)
    {
        CHECK(events[0].type == Type::HeardFrom && events[0].text == "W1AW" && events[0].snr == 12.3);
        CHECK(events[1].type == Type::Missed && events[1].text == "qpsk-r1/3" && events[1].count == 4 &&
              events[1].snr == -2.5);
        CHECK(events[2].type == Type::HeardFrom && events[2].text == "K1ABC" && !events[2].snr);
    }

    // In a session, SN once the partner's burst has likely come.
    CHECK(link.call("W1AW", 100));
    say(link, "OK\rCONNECTED AG7EW W1AW 2300\r", 200);
    link.takeCommandOutput();
    link.takeEvents();
    say(link, "BUFFER 300\r", 300);    // our own bytes going in: no
    CHECK(link.takeCommandOutput().empty());
    say(link, "BUFFER 100\r", 400);    // acknowledged: the partner sent
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    // Not before the partner's first burst, nor from an older host: no refusal.
    say(link, "WRONG\r", 410);
    CHECK(!hasEvent(link.takeEvents(), Type::Refused));

    // At most every SN_GAP_MS: asked again once it has passed.
    link.sessionHeard(500);
    CHECK(link.takeCommandOutput().empty());
    link.tick(400 + HostLink::SN_GAP_MS);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    say(link, "SN 6.5\r", 5500);
    events = link.takeEvents();
    CHECK(events.size() == 1 && events[0].type == Type::SessionSnr && events[0].peer == "W1AW" &&
          events[0].snr == 6.5);
    // The same figure again is the same burst.
    link.sessionHeard(20000);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"SN"}));
    say(link, "SN 6.5\r", 20010);
    CHECK(!hasEvent(link.takeEvents(), Type::SessionSnr));
    // Nothing to ask once the session is over.
    say(link, "DISCONNECTED\r", 21000);
    link.sessionHeard(40000);
    link.tick(40000);
    CHECK(link.takeCommandOutput().empty());
}

void testABurstGoesInItsMode()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}, {0xA0, 2}}, "fsk16r25-r1/3", 100);
    CHECK(id != 0);
    // The mode first, and nothing on the KISS port until the host says OK.
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 fsk16r25-r1/3"}));
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(id) == BurstState::Queued);
    say(link, "OK\r", 200);
    CHECK_EQ(link.groupMode(), std::string("fsk16r25-r1/3"));
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint16_t> tags = tagsWritten(link, 2, &frames);
    CHECK_EQ(tags.size(), (size_t)2);
    CHECK(frames == std::vector<std::vector<uint8_t>>({{0xA0, 1}, {0xA0, 2}}));
    CHECK(link.burstState(id) == BurstState::Sending);
    // The host has it: too late to cancel.
    CHECK(!link.cancel(id));

    // Sent once every tag is back.
    ackTags(link, 2, {tags[0]}, 300);
    CHECK(link.burstState(id) == BurstState::Sending);
    ackTags(link, 2, {tags[1]}, 400);
    CHECK(link.burstState(id) == BurstState::Sent);

    // The next in the same mode goes straight out.
    uint64_t next = link.broadcast({{0xA0, 3}}, "fsk16r25-r1/3", 500);
    CHECK(link.takeCommandOutput().empty());
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
    CHECK(link.burstState(next) == BurstState::Sending);

    // One at a time: a third waits for the second, and can be cancelled.
    uint64_t third = link.broadcast({{0xA0, 4}}, "", 600);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(third) == BurstState::Queued);
    CHECK(link.cancel(third));
    CHECK(link.burstState(third) == BurstState::NotSent);
}

void testAModeTheHostRefuses()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "no-such-mode", 100);
    link.takeCommandOutput();
    say(link, "WRONG\r", 200);
    CHECK(link.burstState(id) == BurstState::NotSent);
    CHECK(link.takeKissOutput().empty());
    CHECK(hasEvent(link.takeEvents(), Type::Refused));
}

void testTheModeWaitsForEarlierAnswers()
{
    // A burst queued before the opening is answered asks for its mode only
    // once every earlier command has its answer, so the OK is the mode's.
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rOK\rOK\rBCAST PORT 2\r", 10);
    uint64_t id = link.broadcast({{0xA0, 1}}, "qpsk-r1/3", 20);
    CHECK(link.takeCommandOutput().empty());
    say(link, "MODE qpsk-r1/3 1200 76 64 1.78 74.36\rOK\r", 30);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 qpsk-r1/3"}));
    say(link, "OK\r", 40);
    CHECK(link.burstState(id) == BurstState::Sending);
}

void testNotSentAfterTwoMinutes()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    tagsWritten(link, 2);
    link.tick(HostLink::NOT_SENT_MS - 1000);
    CHECK(link.burstState(id) == BurstState::Sending);
    link.tick(HostLink::NOT_SENT_MS);
    CHECK(link.burstState(id) == BurstState::NotSent);
    // The next goes once the last is given up.
    uint64_t next = link.broadcast({{0xA0, 2}}, "", HostLink::NOT_SENT_MS + 10);
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
    CHECK(link.burstState(next) == BurstState::Sending);
}

void testASessionHoldsTheGroup()
{
    HostLink link = opened(0);
    say(link, "CONNECTED W1AW AG7EW 2300\r", 0);
    CHECK(link.sessionState() == HostLink::SessionState::Connected);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 1000);
    std::vector<uint16_t> tags = tagsWritten(link, 2);
    // The host has it, but holds it for the session.
    CHECK(link.burstState(id) == BurstState::Held);

    // Not given up however long the session lasts.
    for (uint64_t t = 1000; t <= 20 * 60 * 1000; t += 1000) link.tick(t);
    CHECK(link.burstState(id) == BurstState::Held);

    // The host lets it go in a lull (Data2G PR #59).
    ackTags(link, 2, tags, 20 * 60 * 1000 + 10);
    CHECK(link.burstState(id) == BurstState::Sent);

    // Another held, then the session ends: Sending again, and the two
    // minutes start then.
    uint64_t second = link.broadcast({{0xA0, 2}}, "", 1300000);
    tagsWritten(link, 2);
    CHECK(link.burstState(second) == BurstState::Held);
    say(link, "DISCONNECTED\r", 1400000);
    CHECK(link.burstState(second) == BurstState::Sending);
    link.tick(1400000 + HostLink::NOT_SENT_MS - 1);
    CHECK(link.burstState(second) == BurstState::Sending);
    link.tick(1400000 + HostLink::NOT_SENT_MS);
    CHECK(link.burstState(second) == BurstState::NotSent);
}

void testDroppedAndPortLost()
{
    HostLink link = opened(0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    tagsWritten(link, 2);
    say(link, "BCAST 2 DROPPED 1\r", 10);
    CHECK(link.burstState(id) == BurstState::NotSent);

    uint64_t second = link.broadcast({{0xA0, 2}}, "", 20);
    uint64_t third = link.broadcast({{0xA0, 3}}, "", 20);
    tagsWritten(link, 2);
    say(link, "CONNECTED AG7EW W1AW\r", 30);
    link.takeEvents();
    link.commandDown(40);
    // The one with the host isn't Sent; the one still ours waits.
    CHECK(link.burstState(second) == BurstState::NotSent);
    CHECK(link.burstState(third) == BurstState::Queued);
    CHECK(!link.groupOpen());
    CHECK(link.sessionState() == HostLink::SessionState::None);
    std::vector<HostLink::Event> events = link.takeEvents();
    CHECK(hasEvent(events, Type::GroupClosed));
    bool lost = false;
    for (const auto& e : events) lost = lost || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::HostLost);
    CHECK(lost);

    // Back again, it goes.
    link.commandUp(50);
    link.takeCommandOutput();
    say(link, "OK\rOK\rOK\rOK\rBCAST PORT 3\rOK\r", 60);
    CHECK_EQ(tagsWritten(link, 3).size(), (size_t)1);
    CHECK(link.burstState(third) == BurstState::Sending);
}

void testPauseSending()
{
    HostLink link = opened(0);
    link.setPaused(true, 0);
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 10);
    CHECK(link.takeKissOutput().empty());
    CHECK(link.burstState(id) == BurstState::Queued);
    link.setPaused(false, 20);
    CHECK_EQ(tagsWritten(link, 2).size(), (size_t)1);
}

void testCallingAndLeaving()
{
    HostLink link = opened(0);
    CHECK(link.call("w1aw", 100));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"CONNECT AG7EW W1AW"}));
    CHECK(link.sessionState() == HostLink::SessionState::Calling);
    CHECK(!link.call("K1ABC", 110)); // one session at a time
    say(link, "OK\rCONNECTED AG7EW W1AW 500\r", 200);
    CHECK(link.sessionState() == HostLink::SessionState::Connected);
    CHECK_EQ(link.sessionPeer(), std::string("W1AW"));
    CHECK(!link.sessionIncoming());
    bool connected = false;
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionConnected) connected = e.peer == "W1AW" && !e.incoming && e.number == 500;
    }
    CHECK(connected);

    say(link, "BUFFER 312\r", 300);
    CHECK_EQ(link.buffer(), 312);
    CHECK(hasEvent(link.takeEvents(), Type::Buffer));
    CHECK(link.leave(400));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"DISCONNECT"}));
    CHECK(link.sessionState() == HostLink::SessionState::Leaving);
    // Leave now, while the host waits for the 312 bytes.
    CHECK(link.leaveNow(500));
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"ABORT"}));
    say(link, "OK\rOK\rDISCONNECTED\rBUFFER 0\r", 600);
    CHECK(link.sessionState() == HostLink::SessionState::None);
    CHECK_EQ(link.buffer(), 0);
    HostLink::SessionEnd end = HostLink::SessionEnd::Ended;
    bool bufferAfter = false;
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) end = e.sessionEnd;
        if (e.type == Type::Buffer) bufferAfter = true;
    }
    CHECK(end == HostLink::SessionEnd::Left);
    CHECK(!bufferAfter); // BUFFER 0 after DISCONNECTED is the host listening again

    // No answer.
    CHECK(link.call("K1ABC", 700));
    link.takeEvents();
    say(link, "OK\rDISCONNECTED\r", 800);
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) CHECK(e.sessionEnd == HostLink::SessionEnd::NoAnswer && e.peer == "K1ABC");
    }

    // Called by another station, and that station leaves.
    say(link, "CONNECTED VK3ABC AG7EW\r", 900);
    CHECK(link.sessionIncoming());
    CHECK_EQ(link.sessionPeer(), std::string("VK3ABC"));
    link.takeEvents();
    say(link, "DISCONNECTED\r", 1000);
    for (const auto& e : link.takeEvents())
    {
        if (e.type == Type::SessionEnded) CHECK(e.sessionEnd == HostLink::SessionEnd::Ended);
    }

    // Nothing to leave.
    CHECK(!link.leave(1100));
    CHECK(!link.leaveNow(1100));
    // Leave while calling stops the call.
    CHECK(link.call("W1AW", 1200));
    CHECK(link.leave(1300));
    std::vector<std::string> lines = linesOf(link.takeCommandOutput());
    CHECK(!lines.empty() && lines.back() == "ABORT");
}

void testAnUnansweredCommandIsNotWaitedOnForEver()
{
    // A host that never answers BW2300, say: the burst's BCAST MODE still
    // goes once the wait runs out.
    HostLink link({"AG7EW", true, false});
    link.commandUp(0);
    link.takeCommandOutput();
    say(link, "OK\rOK\rOK\rBCAST PORT 2\r", 10); // one OK short, and no MODES answer
    uint64_t id = link.broadcast({{0xA0, 1}}, "qpsk-r1/3", 20);
    CHECK(link.takeCommandOutput().empty());
    link.tick(HostLink::ANSWER_MS + 20);
    CHECK(linesOf(link.takeCommandOutput()) == std::vector<std::string>({"BCAST MODE 2 qpsk-r1/3"}));
    say(link, "OK\r", HostLink::ANSWER_MS + 30);
    CHECK(link.burstState(id) == BurstState::Sending);
}

void testNothingWithoutTheHost()
{
    HostLink link({"AG7EW", true, false});
    uint64_t id = link.broadcast({{0xA0, 1}}, "", 0);
    CHECK(link.burstState(id) == BurstState::Queued);
    CHECK(link.takeKissOutput().empty());
    CHECK(!link.call("W1AW", 0));
    CHECK(link.broadcast({}, "", 0) == 0);
}

} // namespace

int main()
{
    testTheOpening();
    testReports();
    testSnrReports();
    testABurstGoesInItsMode();
    testAModeTheHostRefuses();
    testTheModeWaitsForEarlierAnswers();
    testNotSentAfterTwoMinutes();
    testASessionHoldsTheGroup();
    testDroppedAndPortLost();
    testPauseSending();
    testCallingAndLeaving();
    testAnUnansweredCommandIsNotWaitedOnForEver();
    testNothingWithoutTheHost();
    return ragtime_test::finish();
}
