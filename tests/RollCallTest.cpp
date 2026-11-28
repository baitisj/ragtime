// Check in by Hand (docs/PROTOCOL.md, "Roll calls"): what a Hand's 63 bits
// say, the RollCall record, and a roll call end to end, with stations
// each a BroadcastChat on its own HostLink, joined by a little host in
// memory that carries frames with their Hand references, keeps each HAND
// SEND and HAND LISTEN, and lands the Hands in the window (two in one
// channel collide). No sockets.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
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
        : link({call, true, narrow}), chat(link), seals(link, chat), db(heardAtDb)
    {
        BroadcastChat::Settings s;
        s.callsign = call;
        s.locator = locator;
        s.software = "Ragtime test";
        chat.setSettings(s, 0);
        stations.setMyCallsign(call);
        // Chat goes unsealed here but in testRepairsByHand.
        seals.setChain(call, broadcast::Seals::Chain());
        seals.setMode("qpsk-r1/3");
        chat.onItems([this](const std::vector<protocol::Record>& items, uint64_t burst, const std::string& mode,
                            uint64_t at) { seals.going(items, burst, mode, at); });
    }

    // Broadcast files too, for the tests that send one.
    void withFiles() { files = std::make_unique<broadcast::BroadcastFiles>(link, chat, seals); }

    void events()
    {
        for (const HostLink::Event& e : link.takeEvents())
        {
            seals.onEvent(e, now);
            if (files) files->onEvent(e, now);
            for (const protocol::Record& r : chat.onEvent(e, now))
            {
                stations.onRecord(r, now);
                seals.onRecord(r, now);
                if (files) files->onRecord(r, now);
                heard.push_back({now, r});
            }
            for (std::vector<protocol::Record> again = seals.takeRebuilt(); !again.empty(); again = seals.takeRebuilt())
            {
                for (const protocol::Record& r : again)
                {
                    chat.onRebuilt(r, now);
                    seals.onRecord(r, now);
                }
            }
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
    broadcast::Seals seals;
    broadcast::Stations stations;
    std::unique_ptr<broadcast::BroadcastFiles> files;
    link::KissDecoder kiss;
    std::vector<std::pair<uint64_t, protocol::Record>> heard;   // every record heard, and when
    // A frame with a record this says is lost isn't heard here.
    std::function<bool(const protocol::Record&)> lose;
    double db;                      // how loud the others hear this station
    bool deaf = false;              // hears nothing on the air
    bool holdBursts = false;        // the host keeps our bursts (the channel busy)
    std::vector<uint8_t> heldKiss;
    std::vector<std::string> notes;

    struct Window
    {
        int tag = 0;
        uint16_t ack = 0;
        std::vector<int> channels;
        bool open = false;
        uint64_t openMs = 0;    // its burst went then
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

// Every burst keyed, and by whom, so a burst over a Hand's slot can spoil
// it: a Hand is some 19 dB under the noise, and a data burst far over it.
struct Keyed
{
    const End* by = nullptr;
    uint64_t atMs = 0;
};
std::vector<Keyed> keyed;
constexpr uint64_t BURST_MS = 1500;     // one codeword, as MODES has it

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
        end->heldKiss.insert(end->heldKiss.end(), out.begin(), out.end());
        if (end->holdBursts) continue;
        out.swap(end->heldKiss);
        end->heldKiss.clear();
        std::vector<link::KissFrame> frames;
        end->kiss.feed(out.data(), out.size(), frames);
        bool sent = false;
        for (const link::KissFrame& f : frames)
        {
            if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
            sent = true;
            uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            if (end->window && tag == end->window->ack && !end->window->open)
            {
                end->window->open = true;
                end->window->openMs = now;
            }
        }
        if (!sent) continue;
        keyed.push_back({end, now});
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
                std::vector<uint8_t> data(f.payload.begin() + 2, f.payload.end());
                if (other->lose)
                {
                    std::vector<protocol::Record> records;
                    protocol::decodeFrame(data, records);
                    if (std::any_of(records.begin(), records.end(), other->lose)) continue;
                }
                std::vector<uint8_t> k = link::kissEncode(GROUP_PORT, data);
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
        end->seals.tick(now);
        if (end->files) end->files->tick(now);
    }
}

// The Hands waiting go into each open window: heard if the tag and
// channel match, two in one channel lost; then the window ends, the delay
// and the Hand after the burst (or now, for a test that stepped there).
void raiseHands(std::vector<End*> ends, bool jump = true)
{
    if (jump) now += (uint64_t)((3.0 + link::HAND_SECONDS + link::HAND_SEARCH_S + 0.4) * 1000.0);
    for (End* net : ends)
    {
        if (!net->window || !net->window->open) continue;
        // The Hands' slot: the delay after its burst, give or take the
        // search. Another station's burst on the air in it spoils them all.
        uint64_t from = net->window->openMs + (uint64_t)((3.0 - link::HAND_SEARCH_S) * 1000.0);
        uint64_t to = net->window->openMs + (uint64_t)((3.0 + link::HAND_SECONDS + link::HAND_SEARCH_S) * 1000.0);
        bool spoilt = std::any_of(keyed.begin(), keyed.end(), [&](const Keyed& k) {
            return k.by != net && k.atMs < to && k.atMs + BURST_MS > from;
        });
        std::map<int, std::vector<End*>> byChannel;
        for (End* end : ends)
        {
            if (end == net || !end->hand) continue;
            if (spoilt || end->hand->tag != net->window->tag) continue;
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
        if (it->kind == Message::Kind::RollCall && it->ours) return &*it;
    }
    return nullptr;
}

// The row of a station's roll call we heard, the newest.
const Message* theirRow(const End& end)
{
    for (auto it = end.chat.messages().rbegin(); it != end.chat.messages().rend(); ++it)
    {
        if (it->kind == Message::Kind::RollCall && !it->ours) return &*it;
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
    // B's list has a row for W1AW's roll call, saying our Hand waits.
    const Message* theirs = theirRow(b);
    CHECK(theirs != nullptr && theirs->from == "W1AW" && theirs->answer == Message::Answer::Waiting);
    if (theirs != nullptr && b.hand)
    {
        CHECK_EQ(theirs->answerChannel, b.hand->channel);
        CHECK(rollCallLine(*theirs)
              == "W1AW called the roll: checking in by Hand (Hand channel " + std::to_string(b.hand->channel) + ")");
    }
    CHECK(rollRow(b) == nullptr);   // not ours
    raiseHands(all);
    settle(all, 2);
    // Once the Hand went, the row says so and stays (Spud, 2026-11-15).
    theirs = theirRow(b);
    CHECK(theirs != nullptr && theirs->answer == Message::Answer::Sent);
    CHECK(theirs != nullptr && rollCallLine(*theirs).find("W1AW called the roll: checked in by Hand") == 0);
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

    // Again: everyone answers every round, those already in too.
    CHECK(a.chat.canCallRoll());
    CHECK(a.chat.checkIn("", now));
    settle(all);
    CHECK(b.hand.has_value() && c.hand.has_value());
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
    // B still shows the roll call, and why it raised no Hand.
    const Message* theirs = theirRow(b);
    CHECK(theirs != nullptr && theirs->answer == Message::Answer::NotAnswered);
    CHECK(theirs != nullptr
          && rollCallLine(*theirs) == "W1AW called the roll: not answered, Check in by Hand is off in Settings");

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

// The host gives an armed window up after 30 s while the check-in waits
// (data2g-host v0.5.1): Ragtime arms it again, and the round goes on.
void testWindowArmedAgain()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    std::vector<End*> all{&a, &b};
    up(all);
    b.chat.checkIn("", now);
    settle(all);
    a.chat.send("Evening all", "", now);
    settle(all);

    a.holdBursts = true;
    a.lines.clear();
    CHECK(a.chat.checkIn("", now));
    settle(all);
    auto listens = [&]() {
        return std::count_if(a.lines.begin(), a.lines.end(),
                             [](const std::string& l) { return l.compare(0, 12, "HAND LISTEN ") == 0; });
    };
    CHECK_EQ(listens(), 1);
    const Message* row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Calling);
    now += 30500;
    a.window.reset();
    a.say("HAND END\r");
    settle(all);
    // Armed again, with the same frame named; the row still waits.
    CHECK_EQ(listens(), 2);
    CHECK(a.window.has_value());
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Calling);

    a.holdBursts = false;
    settle(all);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Listening);
    CHECK(b.hand.has_value());
    raiseHands(all);
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Done && hasHand(row, "KC2G"));
}

// A HAND END too soon after the check-in went, with nothing heard, is the
// armed window giving up, not a round with no Hands: the row says so.
void testWindowThatNeverOpened()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    std::vector<End*> all{&a, &b};
    up(all);
    b.chat.checkIn("", now);
    settle(all);
    a.chat.send("Evening all", "", now);
    settle(all);

    CHECK(a.chat.checkIn("", now));
    settle(all);
    const Message* row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Listening);
    now += 1100;
    a.window.reset();
    a.say("HAND END\r");
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Lost);
    if (row != nullptr)
    {
        CHECK(rollCallLine(*row).find("no window") != std::string::npos);
    }
    // Not still under way: the next one can go.
    CHECK(a.chat.canCallRoll());

    // A window armed again but refused: lost too.
    a.holdBursts = true;
    CHECK(a.chat.checkIn("", now));
    settle(all);
    now += 30500;
    a.say("HAND END\r");
    a.link.takeCommandOutput();     // the HAND LISTEN again, answered here
    a.say("WRONG\r");
    a.events();
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Lost);
    a.holdBursts = false;
    settle(all);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Lost);

    // A host with HAND END EXPIRED (Data2G #200) says so: armed again
    // while the burst waits, and lost after it went, whenever it comes.
    a.holdBursts = true;
    a.lines.clear();
    CHECK(a.chat.checkIn("", now));
    settle(all);
    now += 30500;
    a.say("HAND END EXPIRED\r");
    settle(all);
    CHECK_EQ(std::count_if(a.lines.begin(), a.lines.end(),
                           [](const std::string& l) { return l.compare(0, 12, "HAND LISTEN ") == 0; }),
             2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Calling);
    a.holdBursts = false;
    settle(all);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Listening);
    now += 20000;
    a.window.reset();
    a.say("HAND END EXPIRED\r");
    settle(all, 2);
    row = rollRow(a);
    CHECK(row != nullptr && row->roll == Message::Roll::Lost);
}

} // namespace

// A station that lost a line says so by Hand after the seal, and the
// parity comes with no request burst (docs/PROTOCOL.md, "Repairs by Hand").
void testRepairsByHand()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    a.seals.setSealText(true);
    std::vector<End*> all{&a, &b};
    up(all);
    CHECK(!b.chat.send("Evening all", "", now).empty());    // A's host shows Hands with it
    settle(all);
    CHECK(a.link.handsSeen());
    CHECK(!a.chat.send("Heard by A only", "", now).empty());
    b.deaf = true;
    settle(all);
    b.deaf = false;
    CHECK(!a.chat.send("Heard by both", "", now).empty());
    settle(all);
    // The seal goes after a quiet minute, with a SealRound and a window.
    for (int i = 0; i < 70 && !b.hand; i++)
    {
        now += 1000;
        step(all);
    }
    CHECK(b.hand.has_value());
    CHECK(a.window.has_value());
    if (b.hand)
    {
        std::optional<protocol::RepairHand> hand = protocol::unpackRepairHand(b.hand->payload);
        CHECK(hand.has_value() && hand->call == "KC2G" && hand->itemsWant >= 1 && hand->piecesWant == 0);
        if (a.window) CHECK_EQ(b.hand->tag, a.window->tag);
    }
    raiseHands(all);
    CHECK_EQ(a.seals.handsHeard(), (size_t)1);
    // The round goes a listening window after the Hands, for the requests
    // by burst that wait for them.
    for (int i = 0; i < 90; i++)
    {
        now += 1000;
        step(all);
    }
    // The line came back from parity, and B sent no request.
    bool back = std::any_of(b.chat.messages().begin(), b.chat.messages().end(),
                            [](const Message& m) { return m.text == "Heard by A only"; });
    CHECK(back);
    CHECK_EQ(b.seals.requestsSent(), (size_t)0);
    CHECK(a.seals.paritySent() >= 1);
}

// Issue #185: a file's seal with a Hand window, in a mode whose listening
// window (3 one-codeword bursts and 4 s) ends before the Hand window
// does. The Hands are the round's requests, so the seal isn't sent again
// into the host's hold while they are still to come; the repair round
// follows the window at once; and the station that asked by Hand sends no
// request burst as well.
void testHandWindowOutlastsListening()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    a.withFiles();
    b.withFiles();
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-roll-call-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "voice");
    b.files->setAutoVoice((dir / "voice").string(), 64 * 1024);
    std::vector<End*> all{&a, &b};
    up(all);
    CHECK(!b.chat.send("Evening all", "", now).empty());    // A's host shows Hands with it
    settle(all);
    CHECK(a.link.handsSeen());
    link::ModeInfo mode = broadcast::findMode(a.link.modes(), "qpsk-r1/3");
    uint64_t listening = broadcast::listeningWindowMs(mode);
    uint64_t handWindow = (uint64_t)((3.0 + link::HAND_SECONDS + link::HAND_SEARCH_S) * 1000.0);
    CHECK(listening < handWindow);

    // B loses one piece of the clip the first time it goes.
    auto lost = std::make_shared<bool>(false);
    b.lose = [lost](const protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p == nullptr || p->index != 1 || *lost) return false;
        *lost = true;
        return true;
    };
    std::filesystem::path clip = dir / "hello.sot";
    {
        std::ofstream out(clip, std::ios::binary);
        for (int i = 0; i < 900; i++) out.put((char)(i * 37 + 11));
    }
    std::string error;
    CHECK(a.files->send(clip.string(), "qpsk-r1/3", now, error, false, true) != 0);
    auto sealsFromA = [&]() {
        return std::count_if(b.heard.begin(), b.heard.end(), [](const std::pair<uint64_t, protocol::Record>& h) {
            return h.second.header.callsign == "W1AW" && h.second.as<protocol::Seal>() != nullptr;
        });
    };
    for (int i = 0; i < 120 && !b.hand; i++)
    {
        now += 500;
        step(all);
    }
    // The seal went with its round, and B answers by Hand.
    CHECK(b.hand.has_value());
    CHECK(a.window.has_value() && a.window->open);
    CHECK_EQ(sealsFromA(), (long)1);
    // Both rode in the clip's last burst, after its pieces, not in a
    // keying of their own.
    {
        uint64_t sealAt = 0;
        bool round = false, piece = false;
        for (const auto& h : b.heard)
        {
            if (h.second.header.callsign == "W1AW" && h.second.as<protocol::Seal>() != nullptr) sealAt = h.first;
        }
        for (const auto& h : b.heard)
        {
            if (h.second.header.callsign != "W1AW" || h.first != sealAt) continue;
            round = round || h.second.as<protocol::SealRound>() != nullptr;
            piece = piece || h.second.as<protocol::FilePiece>() != nullptr;
        }
        CHECK(sealAt != 0 && round && piece);
    }

    // The host holds A's bursts while the window is open (Data2G's holds),
    // to its end: nothing of A's should be waiting for it there.
    a.holdBursts = true;
    for (uint64_t t = 0; t < handWindow + 400; t += 500)
    {
        now += 500;
        step(all);
    }
    CHECK(a.heldKiss.empty());
    CHECK(a.link.outgoing().empty());
    raiseHands(all, false);
    a.holdBursts = false;
    CHECK_EQ(a.seals.handsHeard(), (size_t)1);
    size_t from = b.heard.size();
    for (int i = 0; i < 120; i++)
    {
        now += 500;
        step(all);
    }
    // The repair came straight after the window, its parity ahead of the
    // seal, and B never asked by burst.
    const protocol::Record* first = nullptr;
    for (size_t i = from; i < b.heard.size() && first == nullptr; i++)
    {
        if (b.heard[i].second.header.callsign == "W1AW") first = &b.heard[i].second;
    }
    CHECK(first != nullptr && first->as<protocol::FilePiece>() != nullptr);
    CHECK_EQ(b.seals.requestsSent(), (size_t)0);
    bool saved = false;
    for (const broadcast::BroadcastFile& f : b.files->files(now))
        saved = saved || (!f.outgoing && f.state == broadcast::BroadcastFile::State::Saved);
    CHECK(saved);
    std::filesystem::remove_all(dir);
}

// A station that can't answer a seal's round by Hand (here it lacks an
// earlier seal, which a Hand can't name) asks by burst: not over the
// Hands, but after their window, in a listening window there; and the
// seal's sender is still listening then.
void testRequestWaitsForHands()
{
    End a("W1AW", "FN31", 9.0);
    End b("KC2G", "FN20", -6.0);
    End c("K7MHI", "CN85", -3.0);
    a.seals.setSealText(true);
    a.withFiles();
    b.withFiles();
    c.withFiles();
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-roll-call-test-2";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "voice");
    b.files->setAutoVoice((dir / "voice").string(), 64 * 1024);
    // C never hears A's first seal, so A's second names one it lacks.
    c.lose = [](const protocol::Record& r) {
        const protocol::Seal* s = r.as<protocol::Seal>();
        return s != nullptr && r.header.callsign == "W1AW" && s->number == 1;
    };
    std::vector<End*> all{&a, &b, &c};
    up(all);
    CHECK(!b.chat.send("Evening all", "", now).empty());    // A's host shows Hands with it
    settle(all);
    CHECK(!a.chat.send("A line for the net", "", now).empty());
    for (int i = 0; i < 90; i++)
    {
        now += 1000;
        step(all);
    }
    CHECK(a.seals.sealsSent() >= 1);
    raiseHands(all);    // the first seal's window, with no Hands
    settle(all);

    // B loses one piece of the clip the first time it goes, so it answers
    // the round by Hand.
    auto lost = std::make_shared<bool>(false);
    b.lose = [lost](const protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p == nullptr || p->index != 1 || *lost) return false;
        *lost = true;
        return true;
    };
    std::filesystem::path clip = dir / "hello.sot";
    {
        std::ofstream out(clip, std::ios::binary);
        for (int i = 0; i < 900; i++) out.put((char)(i * 37 + 11));
    }
    std::string error;
    CHECK(a.files->send(clip.string(), "qpsk-r1/3", now, error, false, true) != 0);
    size_t cRequests = c.seals.requestsSent();
    for (int i = 0; i < 120 && !b.hand; i++)
    {
        now += 500;
        step(all);
    }
    CHECK(b.hand.has_value());
    CHECK(a.window.has_value() && a.window->open);
    uint64_t opened = a.window ? a.window->openMs : now;
    // What C sent before the window (it may ask for the first seal by
    // its line, three minutes on) is not over the Hands.
    size_t cAtOpen = c.seals.requestsSent();
    uint64_t slotEnds = opened + (uint64_t)((3.0 + link::HAND_SECONDS + link::HAND_SEARCH_S) * 1000.0);
    while (now + 500 < slotEnds)
    {
        now += 500;
        step(all);
    }
    // C kept quiet over the Hands, so B's came through.
    CHECK_EQ(c.seals.requestsSent(), cAtOpen);
    while (now + 500 <= slotEnds + 400)
    {
        now += 500;
        step(all);
    }
    raiseHands(all, false);
    CHECK_EQ(a.seals.handsHeard(), (size_t)1);
    size_t heardBefore = a.seals.requestsHeard();
    for (int i = 0; i < 120; i++)
    {
        now += 500;
        step(all);
    }
    // C asked after the window, and A took it into the round.
    CHECK(c.seals.requestsSent() > cRequests);
    CHECK(a.seals.requestsHeard() > heardBefore);
    for (const Keyed& k : keyed)
    {
        if (k.by == &c && k.atMs > opened) CHECK(k.atMs >= slotEnds);
    }
    bool saved = false;
    for (const broadcast::BroadcastFile& f : b.files->files(now))
        saved = saved || (!f.outgoing && f.state == broadcast::BroadcastFile::State::Saved);
    CHECK(saved);
    std::filesystem::remove_all(dir);
}

void testRepairHandPayload()
{
    protocol::RepairHand hand;
    hand.call = "KC2G";
    hand.snr = -14.0;
    hand.itemsWant = 3;
    hand.piecesWant = 40;
    hand.file = 2;
    uint64_t payload = protocol::packRepairHand(hand);
    std::optional<protocol::RepairHand> back = protocol::unpackRepairHand(payload);
    CHECK(back.has_value());
    if (back)
    {
        CHECK(back->call == "KC2G" && back->snr && *back->snr == -14.0);
        CHECK(back->itemsWant == 3 && back->piecesWant == 40 && back->file == 2);
    }
    // Not a roll call's Hand, nor the other way round.
    CHECK(!protocol::unpackHand(payload).has_value());
    protocol::HandCheckIn checkIn;
    checkIn.call = "KC2G";
    CHECK(!protocol::unpackRepairHand(protocol::packHand(checkIn)).has_value());
    // A tag of its own: seed 1, where a roll call's is 0.
    bool differs = false;
    for (uint16_t round = 1; round < 20; round++)
        differs = differs || protocol::repairTag("W1AW", round) != protocol::rollCallTag("W1AW", round);
    CHECK(differs);
    // The record, both ways.
    protocol::SealRound r;
    r.seal = 77;
    r.channels = 20;
    protocol::Record record;
    record.header.callsign = "W1AW";
    record.header.seq = 5;
    record.header.time = 1792627200;
    record.body = r;
    std::vector<protocol::Record> decoded;
    protocol::decodeFrame(protocol::encodeFrame({record}), decoded);
    CHECK(decoded.size() == 1 && decoded[0].as<protocol::SealRound>() != nullptr);
    if (decoded.size() == 1 && decoded[0].as<protocol::SealRound>() != nullptr)
        CHECK(decoded[0].as<protocol::SealRound>()->seal == 77 && decoded[0].as<protocol::SealRound>()->channels == 20);
}

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
    testWindowArmedAgain();
    testWindowThatNeverOpened();
    testRepairHandPayload();
    testRepairsByHand();
    testHandWindowOutlastsListening();
    testRequestWaitsForHands();
    return ragtime_test::finish();
}
