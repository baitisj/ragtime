// Check in by Hand (docs/PROTOCOL.md, "Roll calls"): what a Hand's 63 bits
// say, the RollCall record, and a roll call end to end, with stations
// each a BroadcastChat on its own HostLink, joined by a little host in
// memory that carries frames with their Hand references, keeps each HAND
// SEND and HAND LISTEN, and lands the Hands in the window (two in one
// channel collide). No sockets.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Hands.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using broadcast::BroadcastChat;
using broadcast::Message;
using link::HostLink;

namespace
{

uint64_t now = 1792627200000; // 2026-10-22 00:00 UTC

constexpr int GROUP_PORT = 2;

//-------------------------------------------------------------------------
// The payload
//-------------------------------------------------------------------------

void testStandardCalls()
{
    for (const char* call : {"W1AW", "AG7EW", "KC2G", "2E0ABC", "VK2ABC", "JA1RT", "G4XYZ", "K1A", "4X1AB"})
    {
        std::optional<uint32_t> packed = protocol::packStandardCall(call);
        CHECK(packed.has_value());
        if (!packed) continue;
        CHECK(*packed < protocol::STANDARD_CALLS);
        CHECK_EQ(protocol::unpackStandardCall(*packed), std::string(call));
    }
    // '/', '-', too long, no digit in place, digits after the digit.
    for (const char* call : {"N0CALL", "AG7EW/P", "AG7EW-1", "VE3/AG7EW", "3DA0XYZ", "ABCDE", "W1", "W12A3", "TEST1"})
        CHECK(!protocol::packStandardCall(call).has_value());
    // Different calls, different numbers.
    CHECK(*protocol::packStandardCall("W1AW") != *protocol::packStandardCall("W1AX"));
    // Numbers no call has: past the end, or not the one way of writing it.
    CHECK(protocol::unpackStandardCall(protocol::STANDARD_CALLS).empty());
    CHECK(protocol::unpackStandardCall(0).empty());     // "  0   ": no letters
}

void testSquares()
{
    CHECK_EQ(protocol::unpackSquare(protocol::packSquare("CN87")), std::string("CN87"));
    CHECK_EQ(protocol::unpackSquare(protocol::packSquare("fn31pr")), std::string("FN31"));
    CHECK_EQ(protocol::unpackSquare(protocol::packSquare("RR99")), std::string("RR99"));
    CHECK_EQ(protocol::packSquare(""), protocol::LOCATOR_NONE);
    CHECK_EQ(protocol::packSquare("ZZ99"), protocol::LOCATOR_NONE);
    CHECK(protocol::unpackSquare(protocol::LOCATOR_NONE).empty());
}

void testHandPayload()
{
    protocol::HandCheckIn hand;
    hand.call = "AG7EW";
    hand.locator = "CN87ts";
    hand.snr = -12.4;
    hand.acceptsSessions = true;
    hand.narrow = true;
    uint64_t bits = protocol::packHand(hand);
    CHECK(bits != 0);
    CHECK((bits >> 63) == 0);
    std::optional<protocol::HandCheckIn> back = protocol::unpackHand(bits);
    CHECK(back.has_value());
    if (back)
    {
        CHECK_EQ(back->call, std::string("AG7EW"));
        CHECK(!back->hash);
        CHECK_EQ(back->locator, std::string("CN87"));
        CHECK(back->snr && *back->snr == -12.0);
        CHECK(back->acceptsSessions && back->narrow && !back->automatic);
    }

    // A call that isn't standard goes as its hash; no locator, no S/N.
    protocol::HandCheckIn odd;
    odd.call = "ag7ew/p";
    odd.snr = 99.0;     // clamped to +32
    std::optional<protocol::HandCheckIn> got = protocol::unpackHand(protocol::packHand(odd));
    CHECK(got.has_value());
    if (got)
    {
        CHECK(got->call.empty());
        CHECK(got->hash && *got->hash == protocol::handCallHash("AG7EW/P"));
        CHECK(got->locator.empty());
        CHECK(got->snr && *got->snr == 32.0);
        CHECK(!got->acceptsSessions);
    }
    protocol::HandCheckIn quiet;
    quiet.call = "W1AW";
    got = protocol::unpackHand(protocol::packHand(quiet));
    CHECK(got && !got->snr && got->locator.empty());

    // Not a callsign: nothing to send.
    protocol::HandCheckIn bad;
    bad.call = "not a call";
    CHECK_EQ(protocol::packHand(bad), (uint64_t)0);

    // Signs of a false decode: spare bits, another version, a call number
    // never sent, a square past RR99.
    CHECK(!protocol::unpackHand(bits | 1));
    CHECK(!protocol::unpackHand(bits | (uint64_t(1) << 60)));
    uint64_t gap = (bits & ~(uint64_t(0x0FFFFFFF) << 32)) | ((uint64_t)(protocol::STANDARD_CALLS + 5) << 32);
    CHECK(!protocol::unpackHand(gap));
    uint64_t pastRR = (bits & ~(uint64_t(0x7FFF) << 17)) | (uint64_t(32500) << 17);
    CHECK(!protocol::unpackHand(pastRR));
}

void testChannels()
{
    // Within the width, and spread: 40 calls don't all share a channel.
    std::set<int> used;
    for (int i = 0; i < 40; i++)
    {
        std::string call = "W" + std::to_string(i % 10) + std::string(1, (char)('A' + i / 10)) + "AA";
        int c = protocol::rollCallChannel(call, 1234, 40);
        CHECK(c >= 0 && c < 40);
        used.insert(c);
        CHECK(protocol::rollCallChannelFits(call, 1234, c, 40));
        // A 500 Hz station answers within its 8, and still fits.
        int narrow = protocol::rollCallChannel(call, 1234, 8);
        CHECK(narrow >= 0 && narrow < 8);
        CHECK(protocol::rollCallChannelFits(call, 1234, narrow, 40));
    }
    CHECK(used.size() > 15);
    // Each round shuffles: some call moves.
    bool moved = false;
    for (const char* call : {"W1AW", "KC2G", "AG7EW", "G4XYZ"})
        moved = moved || protocol::rollCallChannel(call, 1, 40) != protocol::rollCallChannel(call, 2, 40);
    CHECK(moved);
    // The tag is net control's and the round's.
    CHECK(protocol::rollCallTag("W1AW", 7) >= 0 && protocol::rollCallTag("W1AW", 7) <= 255);
    CHECK(protocol::rollCallTag("W1AW", 7) == protocol::rollCallTag("w1aw", 7));
}

void testRecord()
{
    protocol::Record r;
    r.header.callsign = "W1AW";
    r.header.seq = 5;
    r.header.time = 1792627200;
    protocol::RollCall roll;
    roll.round = 0xBEEF;
    roll.delayTenths = 30;
    roll.channels = 40;
    roll.in = {0x1234, protocol::rollCallHash("KC2G")};
    r.body = roll;
    std::vector<uint8_t> frame = protocol::encodeFrame({r});
    CHECK(!frame.empty());
    std::vector<protocol::Record> back;
    CHECK(protocol::decodeFrame(frame, back));
    CHECK_EQ(back.size(), (size_t)1);
    const protocol::RollCall* got = back.empty() ? nullptr : back[0].as<protocol::RollCall>();
    CHECK(got != nullptr);
    if (got != nullptr)
    {
        CHECK_EQ(got->round, (uint16_t)0xBEEF);
        CHECK_EQ(got->delayTenths, (uint8_t)30);
        CHECK_EQ(got->channels, (uint8_t)40);
        CHECK(got->in == roll.in);
    }
    CHECK_EQ(back[0].type(), (uint8_t)protocol::RecordType::RollCall);
    // Round 0 is never sent.
    roll.round = 0;
    r.body = roll;
    CHECK(protocol::encodeFrame({r}).empty());
}

//-------------------------------------------------------------------------
// A roll call on the air
//-------------------------------------------------------------------------

struct End
{
    End(const std::string& call, const std::string& locator, double heardAtDb, bool narrow = false)
        : link({call, true, narrow}), chat(link), db(heardAtDb)
    {
        BroadcastChat::Settings s;
        s.callsign = call;
        s.locator = locator;
        s.software = "Ragtime test";
        chat.setSettings(s, 0);
        stations.setMyCallsign(call);
    }

    void events()
    {
        for (const HostLink::Event& e : link.takeEvents())
        {
            for (const protocol::Record& r : chat.onEvent(e, now)) stations.onRecord(r, now);
        }
        for (const Message::Hand& h : chat.takeHandCheckIns())
        {
            stations.handCheckIn(h.call, h.locator, h.acceptsSessions, h.automatic, now);
            stations.heard(h.call, now, h.snr);
        }
        for (const std::string& n : chat.takeHandNotes()) notes.push_back(n);
    }

    void say(const std::string& text) { link.commandBytes(text.data(), text.size(), now); }

    HostLink link;
    BroadcastChat chat;
    broadcast::Stations stations;
    link::KissDecoder kiss;
    double db;                      // how loud the others hear this station
    bool deaf = false;              // hears nothing on the air
    std::vector<std::string> notes;

    struct Window
    {
        int tag = 0;
        uint16_t ack = 0;
        std::vector<int> channels;
        bool open = false;
    };
    std::optional<Window> window;
    struct Hand
    {
        int ref = 0, tag = 0, channel = 0;
        uint64_t payload = 0;
    };
    std::optional<Hand> hand;
    std::vector<std::string> lines;     // every command line it sent
};

int refs = 0;

std::vector<std::string> words(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// The host in memory, for every end at once.
void step(std::vector<End*> ends)
{
    for (End* end : ends)
    {
        std::string lines = end->link.takeCommandOutput();
        std::string replies;
        size_t at = 0;
        while (at < lines.size())
        {
            size_t cr = lines.find('\r', at);
            std::string line = lines.substr(at, cr - at);
            at = cr + 1;
            end->lines.push_back(line);
            std::vector<std::string> w = words(line);
            if (line.compare(0, 10, "BCAST OPEN") == 0)
                replies += "BCAST PORT " + std::to_string(GROUP_PORT) + "\r";
            else if (line == "MODES")
                replies += "MODE qpsk-r1/3 2300 32 64 1.50 20.00\rOK\r";
            else if (w.size() == 7 && w[0] == "HAND" && w[1] == "SEND")
            {
                end->hand = End::Hand{std::stoi(w[2]), std::stoi(w[3]), std::stoi(w[5]),
                                      std::stoull(w[6], nullptr, 16)};
                replies += "OK\r";
            }
            else if (w.size() >= 6 && w[0] == "HAND" && w[1] == "LISTEN")
            {
                End::Window win;
                win.tag = std::stoi(w[3]);
                win.ack = (uint16_t)std::stoul(w[4], nullptr, 16);
                for (size_t i = 5; i < w.size(); i++) win.channels.push_back(std::stoi(w[i]));
                end->window = win;
                replies += "OK\r";
            }
            else
                replies += "OK\r";
        }
        if (!replies.empty()) end->say(replies);

        std::vector<uint8_t> out = end->link.takeKissOutput();
        std::vector<link::KissFrame> frames;
        end->kiss.feed(out.data(), out.size(), frames);
        bool sent = false;
        for (const link::KissFrame& f : frames)
        {
            if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
            sent = true;
            uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            if (end->window && tag == end->window->ack) end->window->open = true;
        }
        if (!sent) continue;
        int ref = ++refs;
        for (End* other : ends)
        {
            if (other == end || other->deaf) continue;
            char refLine[96];
            std::snprintf(refLine, sizeof refLine, "REF %d 100.00 0.0 %.1f", ref, end->db);
            std::vector<uint8_t> payload{link::KISS_REF};
            payload.insert(payload.end(), refLine, refLine + std::char_traits<char>::length(refLine));
            std::vector<uint8_t> in = link::kissEncodeSetHardware(0, payload);
            for (const link::KissFrame& f : frames)
            {
                if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
                std::vector<uint8_t> k = link::kissEncode(GROUP_PORT, std::vector<uint8_t>(f.payload.begin() + 2, f.payload.end()));
                in.insert(in.end(), k.begin(), k.end());
            }
            other->link.kissBytes(in.data(), in.size(), now);
        }
        for (const link::KissFrame& f : frames)
        {
            if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
            uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            std::vector<uint8_t> ack = link::kissEncodeAckMode(GROUP_PORT, tag, {});
            end->link.kissBytes(ack.data(), ack.size(), now);
        }
    }
    for (End* end : ends)
    {
        end->link.tick(now);
        end->events();
        end->chat.tick(now);
    }
}

// The Hands waiting go into each open window: heard if the tag and
// channel match, two in one channel lost; then the window ends.
void raiseHands(std::vector<End*> ends)
{
    for (End* net : ends)
    {
        if (!net->window || !net->window->open) continue;
        std::map<int, std::vector<End*>> byChannel;
        for (End* end : ends)
        {
            if (end == net || !end->hand) continue;
            if (end->hand->tag != net->window->tag) continue;
            if (std::find(net->window->channels.begin(), net->window->channels.end(), end->hand->channel)
                == net->window->channels.end())
                continue;
            byChannel[end->hand->channel].push_back(end);
        }
        std::string replies;
        for (const auto& [channel, in] : byChannel)
        {
            if (in.size() != 1) continue;
            char line[96];
            std::snprintf(line, sizeof line, "HAND %d %016llx %.1f 20\r", channel,
                          (unsigned long long)in[0]->hand->payload, in[0]->db);
            replies += line;
        }
        replies += "HAND END\r";
        net->say(replies);
        net->window.reset();
    }
    for (End* end : ends)
    {
        if (!end->hand) continue;
        end->say("HAND SENT " + std::to_string(end->hand->channel) + "\r");
        end->hand.reset();
    }
    for (End* end : ends)
    {
        end->events();
    }
}

void settle(std::vector<End*> ends, int steps = 8)
{
    for (int i = 0; i < steps; i++)
    {
        now += 100;
        step(ends);
    }
}

void up(std::vector<End*> ends)
{
    for (End* end : ends) end->link.commandUp(now);
    settle(ends);
}

const Message* rollRow(const End& end)
{
    for (auto it = end.chat.messages().rbegin(); it != end.chat.messages().rend(); ++it)
    {
        if (it->kind == Message::Kind::RollCall) return &*it;
    }
    return nullptr;
}

bool hasHand(const Message* row, const std::string& call)
{
    if (row == nullptr) return false;
    return std::any_of(row->hands.begin(), row->hands.end(), [&](const Message::Hand& h) { return h.call == call; });
}

void testCheckInCallsTheRoll()
{
    End a("W1AW", "FN31pr", 9.0);
    End b("KC2G", "FN20", -6.0);
    End c("G4XYZ", "IO91wm", 3.0, true);    // 500 Hz: 8 channels
    std::vector<End*> all{&a, &b, &c};
    up(all);

    // Before any burst heard, the host hasn't shown Hands: a plain check-in.
    CHECK(!a.chat.canCallRoll());
    CHECK(!a.chat.whyNoRollCall().empty());
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(rollRow(a) == nullptr);
    // B's message is heard with a reference, so A's host has shown Hands.
    CHECK(!b.chat.send("Evening all", "", now).empty());
    settle(all);
    CHECK(a.link.handsSeen());
    CHECK(a.chat.canCallRoll());

    // A checks in again: Hello, RollCall and a window over all 40.
    b.lines.clear();
    c.lines.clear();
    CHECK(a.chat.checkIn("", now));
    const Message* row = rollRow(a);
    CHECK(row != nullptr && row->ours && row->rollChannels == 40);
    CHECK(!a.chat.canCallRoll());   // one at a time
    settle(all);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Listening);
    bool listened = std::any_of(a.lines.begin(), a.lines.end(),
                                [](const std::string& l) { return l.compare(0, 14, "HAND LISTEN 3.") == 0; });
    CHECK(listened);
    // B and C answered by themselves, each in its round's channel.
    CHECK(b.hand.has_value() && c.hand.has_value());
    if (b.hand && c.hand && row != nullptr)
    {
        CHECK_EQ(b.hand->channel, protocol::rollCallChannel("KC2G", row->round, 40));
        CHECK_EQ(c.hand->channel, protocol::rollCallChannel("G4XYZ", row->round, 8));
        CHECK_EQ(b.hand->tag, protocol::rollCallTag("W1AW", row->round));
    }
    bool collide = b.hand && c.hand && b.hand->channel == c.hand->channel;
    CHECK(!collide);    // not with these calls and this round
    raiseHands(all);
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Done);
    if (!collide)
    {
        CHECK(hasHand(row, "KC2G") && hasHand(row, "G4XYZ"));
        CHECK(row != nullptr && row->hands.size() == 2);
        const broadcast::Station* s = a.stations.find("G4XYZ");
        CHECK(s != nullptr && s->handMs != 0 && s->locator == "IO91" && s->snrDb && *s->snrDb == 3.0);
        for (const Message::Hand& h : row->hands)
        {
            if (h.call == "G4XYZ") CHECK(h.narrow && h.theyHear && *h.theyHear == 9.0 && h.locator == "IO91");
        }
        CHECK(rollCallLine(*row).find("2 checked in by Hand") != std::string::npos);
    }
    CHECK(!b.notes.empty() && b.notes.back().find("W1AW's roll call") != std::string::npos);

    // Again: those in stay quiet, so nobody answers this round.
    CHECK(a.chat.canCallRoll());
    CHECK(a.chat.checkIn("", now));
    settle(all);
    if (!collide) CHECK(!b.hand && !c.hand);
    raiseHands(all);
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Done);
}

void testWhoStaysQuiet()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    std::vector<End*> all{&a, &b};
    up(all);
    b.chat.checkIn("", now);
    settle(all);
    a.chat.send("Evening all", "", now);    // B's host shows Hands with it
    settle(all);

    // Answering turned off: no Hand.
    BroadcastChat::Settings s = b.chat.settings();
    s.answerRollCalls = false;
    b.chat.setSettings(s, b.chat.newestItem());
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(!b.hand);
    raiseHands(all);
    settle(all, 2);
    const Message* row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Done && row->hands.empty());
    CHECK(rollCallLine(*row) == "Roll call: no Hands heard");

    // A Hand whose call is in the wrong channel is a false decode.
    s.answerRollCalls = true;
    b.chat.setSettings(s, b.chat.newestItem());
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(b.hand.has_value());
    row = rollRow(a);
    if (b.hand && row != nullptr)
    {
        int wrong = (protocol::rollCallChannel("KC2G", row->round, 40) + 1) % 40;
        if (protocol::rollCallChannel("KC2G", row->round, 8) == wrong) wrong = (wrong + 1) % 40;
        b.hand->channel = wrong;
    }
    raiseHands(all);
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Done && row->hands.empty());
}

void testHashedCall()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G/P", "FN20", -6.0);
    std::vector<End*> all{&a, &b};
    up(all);
    b.chat.checkIn("", now);    // A hears KC2G/P's call, so its hash can be put back
    settle(all);
    a.chat.send("Evening all", "", now);
    settle(all);
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(b.hand.has_value());
    raiseHands(all);
    settle(all, 2);
    CHECK(hasHand(rollRow(a), "KC2G/P"));

    // A call never heard comes as its hash, in words.
    const Message* row = rollRow(a);
    CHECK(row != nullptr && row->hands.size() == 1);
    if (row != nullptr && row->hands.size() == 1)
    {
        Message m = *row;
        m.hands[0].call.clear();
        m.hands[0].hash = 0x1a2b3c;
        CHECK(rollCallLine(m).find("a call we haven't heard (#1a2b3c)") != std::string::npos);
    }
}

void testOneHandAtATime()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    End c("G4XYZ", "IO91", 3.0);
    std::vector<End*> all{&a, &b, &c};
    up(all);
    for (End* e : all)
    {
        e->chat.send("Evening all", "", now);
        settle(all);
    }
    // A and C call the roll together: B's Hand for A waits, so C's goes
    // unanswered, and the status bar says why.
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(b.hand.has_value() && b.hand->tag == protocol::rollCallTag("W1AW", rollRow(a)->round));
    CHECK(c.chat.checkIn("", now));
    settle(all);
    CHECK(b.hand && b.hand->tag == protocol::rollCallTag("W1AW", rollRow(a)->round));
    CHECK(!b.notes.empty() && b.notes.back().find("one at a time") != std::string::npos);
}

void testNotInARagchew()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    std::vector<End*> all{&a, &b};
    up(all);
    b.chat.checkIn("", now);
    settle(all);
    a.chat.send("Evening all", "", now);
    settle(all);
    // B is in a session with someone else.
    CHECK(b.link.call("JA1RT", now));
    settle(all);
    b.say("CONNECTED KC2G JA1RT 2300\r");
    settle(all);
    CHECK(b.link.sessionState() == HostLink::SessionState::Connected);
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(!b.hand);
}

} // namespace

int main()
{
    testStandardCalls();
    testSquares();
    testHandPayload();
    testChannels();
    testRecord();
    testCheckInCallsTheRoll();
    testWhoStaysQuiet();
    testHashedCall();
    testOneHandAtATime();
    testNotInARagchew();
    return ragtime_test::finish();
}
