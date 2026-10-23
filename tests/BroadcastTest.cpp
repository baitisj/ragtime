// The Broadcast tab's engine and the station list: two stations, each a
// BroadcastChat on its own HostLink, joined by a little host in memory
// that answers the command lines and carries every ACKMODE frame to the
// other station. No sockets, so it runs everywhere the core builds.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastLog.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/link/Reconnect.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using broadcast::BroadcastChat;
using broadcast::Column;
using broadcast::Message;
using broadcast::Stations;
using link::HostLink;
using State = Message::State;

namespace
{

uint64_t now = 1792627200000; // 2026-10-22 00:00 UTC

constexpr int GROUP_PORT = 2;

struct End
{
    explicit End(const std::string& call, const std::string& locator) : link({call, true, false}), chat(link)
    {
        BroadcastChat::Settings s;
        s.callsign = call;
        s.locator = locator;
        s.software = "Ragtime test";
        chat.setSettings(s, 0);
        stations.setMyCallsign(call);
        stations.setHome(locator);
    }

    // Hands every event to the chat and the station list, as the app does.
    void events()
    {
        for (const HostLink::Event& e : link.takeEvents())
        {
            if (e.type == HostLink::Event::Type::HeardFrom) stations.heard(e.text, now);
            for (const protocol::Record& r : chat.onEvent(e, now))
            {
                stations.onRecord(r, now);
                heard.push_back(r);
            }
        }
    }

    HostLink link;
    BroadcastChat chat;
    Stations stations;
    link::KissDecoder kiss;
    std::vector<protocol::Record> heard;
    bool hold = false;          // the host keeps our frames
    bool dropNext = false;      // the host drops our next frames
};

// The host in memory: OK to everything, the group's port, a MODES list,
// and every ACKMODE frame carried to the other end and acknowledged.
void step(End& a, End& b)
{
    for (End* end : {&a, &b})
    {
        End& other = end == &a ? b : a;
        std::string lines = end->link.takeCommandOutput();
        std::string replies;
        size_t at = 0;
        while (at < lines.size())
        {
            size_t cr = lines.find('\r', at);
            std::string line = lines.substr(at, cr - at);
            at = cr + 1;
            if (line.compare(0, 10, "BCAST OPEN") == 0)
                replies += "BCAST PORT " + std::to_string(GROUP_PORT) + "\r";
            else if (line == "MODES")
                replies += "MODE qpsk-r1/3 2300 32 64 1.50 20.00\rMODE fsk16r25-r1/3 500 8 32 4.00 60.00\rOK\r";
            else
                replies += "OK\r";
        }
        if (!replies.empty()) end->link.commandBytes(replies.data(), replies.size(), now);

        if (end->hold) continue;
        std::vector<uint8_t> out = end->link.takeKissOutput();
        std::vector<link::KissFrame> frames;
        end->kiss.feed(out.data(), out.size(), frames);
        if (frames.empty()) continue;
        if (end->dropNext)
        {
            end->dropNext = false;
            std::string dropped = "BCAST " + std::to_string(GROUP_PORT) + " DROPPED 1\r";
            end->link.commandBytes(dropped.data(), dropped.size(), now);
            continue;
        }
        std::string heard = "PTT ON\rMODE qpsk-r1/3\r";
        end->link.commandBytes(heard.data(), heard.size(), now);
        std::string from = "BCAST " + std::to_string(GROUP_PORT) + " HEARD " + end->link.settings().callsign + "\r";
        other.link.commandBytes(from.data(), from.size(), now);
        for (const link::KissFrame& f : frames)
        {
            if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
            std::vector<uint8_t> data(f.payload.begin() + 2, f.payload.end());
            CHECK(data.size() <= protocol::MAX_BROADCAST_FRAME);
            std::vector<uint8_t> kiss = link::kissEncode(GROUP_PORT, data);
            other.link.kissBytes(kiss.data(), kiss.size(), now);
            uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            std::vector<uint8_t> ack = link::kissEncodeAckMode(GROUP_PORT, tag, {});
            end->link.kissBytes(ack.data(), ack.size(), now);
        }
        std::string off = "PTT OFF\r";
        end->link.commandBytes(off.data(), off.size(), now);
    }
    a.link.tick(now);
    b.link.tick(now);
    a.events();
    b.events();
}

void settle(End& a, End& b, int steps = 8)
{
    for (int i = 0; i < steps; i++)
    {
        now += 100;
        step(a, b);
    }
}

const Message* last(const End& end)
{
    return end.chat.messages().empty() ? nullptr : &end.chat.messages().back();
}

bool heardHello(const End& end)
{
    for (const protocol::Record& r : end.heard)
    {
        if (r.as<protocol::Hello>() != nullptr) return true;
    }
    return false;
}

void testAddressee()
{
    std::string call, rest;
    CHECK(BroadcastChat::addressee("@w1aw hello there", call, rest));
    CHECK_EQ(call, std::string("W1AW"));
    CHECK_EQ(rest, std::string("hello there"));
    CHECK(BroadcastChat::addressee("  @KC2G: are you there?", call, rest));
    CHECK_EQ(call, std::string("KC2G"));
    CHECK_EQ(rest, std::string("are you there?"));
    CHECK(!BroadcastChat::addressee("@W1AW", call, rest));      // nothing to say
    CHECK(!BroadcastChat::addressee("hello @W1AW", call, rest));
    CHECK(!BroadcastChat::addressee("@ hello", call, rest));
    CHECK(!BroadcastChat::addressee("@W1AW_X hi", call, rest)); // not a callsign
}

void testPhrasesTyped()
{
    using ragtime::text::HamText::PhraseSpan;
    auto has = [](const std::vector<PhraseSpan>& spans, size_t start, size_t length) {
        for (const PhraseSpan& span : spans)
            if (span.start == start && span.length == length) return true;
        return false;
    };
    // Unaddressed: the phrases of the whole line.
    std::vector<PhraseSpan> spans = BroadcastChat::phrases("CQ CQ de AG7EW");
    CHECK(has(spans, 0, 5));
    std::vector<PhraseSpan> whole = ragtime::text::HamText::phraseSpans("CQ CQ de AG7EW");
    CHECK_EQ(spans.size(), whole.size());
    for (size_t i = 0; i < spans.size() && i < whole.size(); i++)
        CHECK(spans[i].start == whole[i].start && spans[i].length == whole[i].length);
    // Addressed: the "@W1AW, " goes in the To, so only the rest is coded,
    // and the spans stay where the operator sees them.
    spans = BroadcastChat::phrases("@W1AW, CQ CQ");
    CHECK_EQ(spans.size(), (size_t)1);
    CHECK(has(spans, 7, 5));
    // UTF-8 before a phrase: offsets are bytes, and no phrase starts inside
    // a character.
    spans = BroadcastChat::phrases("\xc3\xa9t\xc3\xa9 CQ CQ");
    CHECK(has(spans, 6, 5));
    CHECK(BroadcastChat::phrases("").empty());
}

void testMessageBothWays()
{
    End a("TEST1", "CN87ux"), b("TEST2", "FN31pr");
    a.link.commandUp(now);
    b.link.commandUp(now);
    settle(a, b);
    CHECK(a.link.groupOpen());
    CHECK(b.link.groupOpen());

    std::vector<uint64_t> ids = a.chat.send("CQ CQ de TEST1", "", now);
    CHECK_EQ(ids.size(), (size_t)1);
    const Message* mine = a.chat.find(ids[0]);
    CHECK(mine != nullptr && mine->ours);
    settle(a, b);
    CHECK(mine->state == State::Sent);
    CHECK_EQ(mine->mode, std::string("qpsk-r1/3"));
    CHECK_EQ(a.chat.newestItem(), (uint32_t)1);

    const Message* heard = last(b);
    CHECK(heard != nullptr);
    if (heard)
    {
        CHECK_EQ(heard->from, std::string("TEST1"));
        CHECK_EQ(heard->text, std::string("CQ CQ de TEST1"));
        CHECK(heard->state == State::Heard);
        CHECK_EQ(heard->seq, (uint32_t)1);
    }
    // The first message carried a Hello: the station list has the locator.
    CHECK(heardHello(b));
    const broadcast::Station* s = b.stations.find("TEST1");
    CHECK(s != nullptr);
    if (s)
    {
        CHECK_EQ(s->locator, std::string("CN87ux"));
        CHECK_EQ(s->software, std::string("Ragtime test"));
        CHECK_EQ(s->texts, (size_t)1);
        CHECK_EQ(b.stations.cell(*s, Column::Status, now), std::string("\xe2\x97\x8f QRV"));
        CHECK_EQ(b.stations.cell(*s, Column::Locator, now), std::string("CN87ux"));
        CHECK(!b.stations.cell(*s, Column::Distance, now).empty());
    }
    CHECK(b.stations.find("TEST2") == nullptr);     // never ourselves

    // The next one within 10 minutes goes without a Hello.
    b.heard.clear();
    a.chat.send("second", "", now);
    settle(a, b);
    CHECK(!heardHello(b));
    CHECK_EQ(b.chat.messages().size(), (size_t)2);

    // Ten minutes on, it says hello again.
    now += broadcast::HELLO_EVERY_MS;
    b.heard.clear();
    a.chat.send("third", "", now);
    settle(a, b);
    CHECK(heardHello(b));

    // A change of locator says hello at once.
    b.heard.clear();
    BroadcastChat::Settings settings = a.chat.settings();
    settings.locator = "CN88";
    a.chat.setSettings(settings, a.chat.newestItem());
    a.chat.send("moved", "", now);
    settle(a, b);
    CHECK(heardHello(b));
    s = b.stations.find("TEST1");
    CHECK(s != nullptr && s->locator == "CN88");
    CHECK_EQ(a.chat.newestItem(), (uint32_t)4);
}

void testAddressedAndAcknowledged()
{
    End a("TEST1", "CN87"), b("TEST2", "FN31");
    a.link.commandUp(now);
    b.link.commandUp(now);
    settle(a, b);

    std::vector<uint64_t> ids = a.chat.send("@test2 are you there?", "", now);
    CHECK_EQ(ids.size(), (size_t)1);
    const Message* mine = a.chat.find(ids[0]);
    CHECK(mine != nullptr && mine->to == "TEST2" && mine->ackWanted);
    CHECK_EQ(mine->text, std::string("are you there?"));
    settle(a, b);
    // TEST2 heard it addressed to it, and acknowledged it on the group.
    const Message* heard = last(b);
    CHECK(heard != nullptr && heard->to == "TEST2" && heard->text == "are you there?");
    CHECK(mine->state == State::Acknowledged);

    // Re-sent once acknowledged: TEST2 acknowledges it again, and doesn't
    // list it twice.
    size_t listed = b.chat.messages().size();
    CHECK(a.chat.resend(ids[0], "", now));
    CHECK(mine->state == State::Queued);
    settle(a, b);
    CHECK(mine->state == State::Acknowledged);
    CHECK_EQ(b.chat.messages().size(), listed);

    // A message for someone else gets no Ack from TEST2.
    ids = a.chat.send("@W1AW hello", "", now);
    settle(a, b);
    CHECK(a.chat.find(ids[0])->state == State::Sent);
}

void testLongMessageSplits()
{
    End a("TEST1", "CN87"), b("TEST2", "FN31");
    a.link.commandUp(now);
    b.link.commandUp(now);
    settle(a, b);

    std::string text;
    while (text.size() < 1500) text += "the quick brown fox jumps over the lazy dog, \xc3\xa9t\xc3\xa9 ";
    text.resize(1500);
    std::vector<uint64_t> ids = a.chat.send(text, "", now);
    CHECK(ids.size() >= 3);
    settle(a, b, 20);
    std::string joined;
    size_t parts = 0;
    for (const Message& m : b.chat.messages())
    {
        if (!joined.empty()) joined += m.text.empty() || text[joined.size()] != ' ' ? "" : " ";
        joined += m.text;
        parts++;
    }
    CHECK_EQ(parts, ids.size());
    CHECK_EQ(joined, text);
    for (uint64_t id : ids) CHECK(a.chat.find(id)->state == State::Sent);

    CHECK(a.chat.send(std::string(broadcast::MAX_TYPED + 1, 'x'), "", now).empty());
    CHECK(a.chat.send("   ", "", now).empty());
}

void testPauseCancelResend()
{
    End a("TEST1", "CN87"), b("TEST2", "FN31");
    a.link.commandUp(now);
    b.link.commandUp(now);
    settle(a, b);

    a.link.setPaused(true, now);
    std::vector<uint64_t> ids = a.chat.send("held back", "", now);
    settle(a, b);
    const Message* m = a.chat.find(ids[0]);
    CHECK(m->state == State::Queued);
    CHECK(a.chat.cancel(ids[0]));
    settle(a, b);
    CHECK(m->state == State::Cancelled);
    CHECK(b.chat.messages().empty());

    a.link.setPaused(false, now);
    CHECK(a.chat.resend(ids[0], "", now));
    settle(a, b);
    CHECK(m->state == State::Sent);
    CHECK_EQ(b.chat.messages().size(), (size_t)1);

    // Dropped by the host: Not sent; sent again it is the same item, and a
    // station that heard the first copy doesn't list it twice.
    a.dropNext = true;
    ids = a.chat.send("dropped", "", now);
    settle(a, b);
    m = a.chat.find(ids[0]);
    CHECK(m->state == State::NotSent);
    CHECK(a.chat.resend(ids[0], "", now));
    settle(a, b);
    CHECK(m->state == State::Sent);
    CHECK_EQ(b.chat.messages().size(), (size_t)2);
    // Re-sent once Sent: still one line at TEST2.
    CHECK(a.chat.resend(ids[0], "", now));
    CHECK(a.chat.resend(ids[0], "", now) == false);     // not while it is still going
    settle(a, b);
    CHECK(m->state == State::Sent);
    CHECK_EQ(b.chat.messages().size(), (size_t)2);

    // A burst the host holds too long is Not sent.
    a.hold = true;
    ids = a.chat.send("into the void", "", now);
    settle(a, b);
    m = a.chat.find(ids[0]);
    CHECK(m->state == State::Sending);
    now += HostLink::NOT_SENT_MS + 1000;
    settle(a, b);
    CHECK(m->state == State::NotSent);
}

void testHistory()
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-broadcast-history-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::string path = (dir / "broadcast.log").string();

    uint32_t newest = 0;
    {
        End a("TEST1", "CN87"), b("TEST2", "FN31");
        broadcast::BroadcastLog log(path);
        a.chat.setLog(&log, now);
        CHECK(a.chat.messages().empty());
        a.link.commandUp(now);
        b.link.commandUp(now);
        settle(a, b);
        a.chat.send("CQ CQ de TEST1", "", now);
        settle(a, b);
        b.chat.send("@TEST1 gm, QRV", "", now);
        settle(a, b);
        a.chat.addFile(7, false, "TEST2", "net.txt", now);
        a.chat.fileEnded(7, "Saved");
        a.dropNext = true;
        a.chat.send("lost in the noise", "", now);
        settle(a, b);
        CHECK_EQ(a.chat.messages().size(), (size_t)4);
        newest = a.chat.newestItem();
        now += 60000;
        a.chat.closeLog(now);
    }

    // Ragtime opens again: what was said, greyed, a rule, and on below.
    End a("TEST1", "CN87"), b("TEST2", "FN31");
    BroadcastChat::Settings s = a.chat.settings();
    a.chat.setSettings(s, newest);
    broadcast::BroadcastLog log(path);
    now += 60000;
    a.chat.setLog(&log, now);
    const auto& rows = a.chat.messages();
    CHECK_EQ(rows.size(), (size_t)5);
    if (rows.size() == 5)
    {
        CHECK(rows[0].past && rows[0].ours && rows[0].state == State::Sent);
        CHECK_EQ(rows[0].text, std::string("CQ CQ de TEST1"));
        CHECK(rows[1].past && !rows[1].ours && rows[1].to == "TEST1" && rows[1].ackWanted);
        CHECK(rows[2].kind == Message::Kind::File && rows[2].outcome == "Saved" && rows[2].file == 0);
        CHECK(rows[3].state == State::NotSent);
        CHECK(rows[4].kind == Message::Kind::Rule);
        CHECK(a.chat.canResend(rows[3]));
        CHECK(!a.chat.canResend(rows[1]) && !a.chat.canResend(rows[2]) && !a.chat.canResend(rows[4]));
    }
    a.link.commandUp(now);
    b.link.commandUp(now);
    settle(a, b);

    // TEST2 says it again: not listed twice.
    b.chat.send("@TEST1 gm, QRV", "", now);
    settle(a, b);
    CHECK_EQ(a.chat.messages().size(), (size_t)6);
    CHECK_EQ(a.chat.messages().back().text, std::string("gm, QRV"));
    CHECK(!a.chat.messages().back().past);

    // Our lost line from last time goes again, as the same item.
    uint64_t lost = rows[3].id;
    CHECK(a.chat.resend(lost, "", now));
    settle(a, b);
    const Message* m = a.chat.find(lost);
    CHECK(m != nullptr && m->state == State::Sent && !m->past);
    const Message* there = last(b);
    CHECK(there != nullptr && there->text == "lost in the noise" && there->seq == newest);

    // A past line of ours under another callsign stays put.
    s.callsign = "TEST3";
    a.chat.setSettings(s, 0);
    CHECK(!a.chat.canResend(rows[0]));

    // This run's lines are in the log for next time, the re-sent one too.
    a.chat.closeLog(now);
    std::vector<Message> again = log.load(100);
    CHECK_EQ(again.size(), (size_t)8);
    if (again.size() == 8)
    {
        CHECK(again[4].kind == Message::Kind::Rule);
        CHECK_EQ(again[5].text, std::string("gm, QRV"));
        CHECK_EQ(again[6].text, std::string("lost in the noise"));
        CHECK(again[6].state == State::Sent);
        CHECK(again[7].kind == Message::Kind::Rule);
    }
    std::filesystem::remove_all(dir, ec);
}

void testDuplicateFrame()
{
    End a("TEST1", "CN87"), b("TEST2", "FN31");
    b.link.commandUp(now);
    settle(a, b);
    protocol::Numbering numbering("W1AW");
    protocol::Text text;
    text.text = "QST QST";
    std::vector<uint8_t> frame = protocol::encodeFrame({numbering.record(text, 1792627200)});
    std::vector<uint8_t> kiss = link::kissEncode(GROUP_PORT, frame);
    b.link.kissBytes(kiss.data(), kiss.size(), now);
    b.link.kissBytes(kiss.data(), kiss.size(), now);
    std::vector<uint8_t> foreign = link::kissEncode(GROUP_PORT, {0x55, 0x01, 0x02});
    b.link.kissBytes(foreign.data(), foreign.size(), now);
    b.events();
    CHECK_EQ(b.chat.messages().size(), (size_t)1);
    CHECK_EQ(b.chat.foreignFrames(), (size_t)1);
    CHECK(b.stations.find("W1AW") != nullptr);
}

void testStations()
{
    using broadcast::heardAgo;
    CHECK_EQ(heardAgo(0, 5000), std::string("just now"));
    CHECK_EQ(heardAgo(0, 45000), std::string("45 s ago"));
    CHECK_EQ(heardAgo(0, 12 * 60000 + 5000), std::string("12 min ago"));
    CHECK_EQ(heardAgo(0, 125 * 60000), std::string("2 h 5 min ago"));
    CHECK_EQ(heardAgo(0, 3 * 3600000), std::string("3 h ago"));
    CHECK_EQ(heardAgo(0, 75ull * 3600000), std::string("3 days ago"));

    Stations list;
    list.setMyCallsign("AG7EW");
    list.setHome("CN87ux");
    uint64_t t = 1000000;

    auto hello = [&](const std::string& call, const std::string& locator, bool sessions) {
        protocol::Numbering n(call);
        protocol::Hello h;
        h.locator = locator;
        h.acceptsSessions = sessions;
        list.onRecord(n.record(h, 0), t);
    };
    auto status = [&](const std::string& call, protocol::Status::State state, const std::string& partner) {
        protocol::Numbering n(call);
        protocol::Status s;
        s.state = state;
        s.partner = partner;
        list.onRecord(n.record(s, 0), t);
    };

    hello("W1AW", "FN31pr", true);
    t += 1000;
    hello("KC2G", "FN20", false);
    t += 1000;
    list.heard("JA1XYZ", t);                    // no Hello yet
    t += 1000;
    hello("AG7EW", "CN87", true);               // ourselves: never listed
    CHECK_EQ(list.stations().size(), (size_t)3);

    const broadcast::Station* w1aw = list.find("w1aw");
    CHECK(w1aw != nullptr);
    CHECK_EQ(list.cell(*w1aw, Column::Distance, t), std::string("3,900 km"));
    CHECK_EQ(list.cell(*w1aw, Column::Bearing, t), std::string("81\xc2\xb0"));
    CHECK_EQ(list.whereLine(*w1aw), std::string("FN31pr \xc2\xb7 3,900 km"));
    CHECK_EQ(list.whereLine(*list.find("JA1XYZ")), std::string("no locator"));
    CHECK_EQ(list.status(*list.find("KC2G"), t).word, std::string("No sessions"));
    CHECK(!list.status(*list.find("KC2G"), t).filled);

    // A QRL names the partner; stale after 20 minutes; QRV clears it.
    status("W1AW", protocol::Status::State::Qrl, "KC2G");
    CHECK_EQ(list.status(*w1aw, t).word, std::string("QSO w/ KC2G"));
    CHECK_EQ(list.cell(*w1aw, Column::Status, t + 25 * 60000), std::string("\xe2\x97\x8b QRL? 25 min ago"));
    status("W1AW", protocol::Status::State::Qrv, "");
    CHECK_EQ(list.status(*w1aw, t).word, std::string("QRV"));
    status("W1AW", protocol::Status::State::Qrt, "");
    CHECK_EQ(list.status(*w1aw, t).word, std::string("QRT"));
    t += 1000;
    protocol::Numbering n("W1AW");
    protocol::Text text;
    text.text = "back again";
    list.onRecord(n.record(text, 0), t);
    CHECK_EQ(list.status(*w1aw, t).word, std::string("QRV"));

    // Sorting: Heard newest first; Distance nearest first, no locator last
    // either way; Call alphabetical.
    std::vector<const broadcast::Station*> s = list.sorted(Column::Heard, true, t);
    CHECK_EQ(s[0]->call, std::string("W1AW"));
    CHECK_EQ(s[1]->call, std::string("JA1XYZ"));
    s = list.sorted(Column::Distance, true, t);
    CHECK_EQ(s[0]->call, std::string("KC2G"));     // FN20 is a little nearer CN87 than FN31
    CHECK_EQ(s[1]->call, std::string("W1AW"));
    CHECK_EQ(s[2]->call, std::string("JA1XYZ"));
    s = list.sorted(Column::Distance, false, t);
    CHECK_EQ(s[0]->call, std::string("W1AW"));
    CHECK_EQ(s[2]->call, std::string("JA1XYZ"));
    s = list.sorted(Column::Call, true, t);
    CHECK_EQ(s[0]->call, std::string("JA1XYZ"));
    CHECK_EQ(s[2]->call, std::string("W1AW"));
    s = list.sorted(Column::Status, true, t);
    CHECK_EQ(s[2]->call, std::string("KC2G"));      // No sessions after QRV

    // Columns by key; every column shows at first.
    Column c = Column::Call;
    CHECK(broadcast::columnFromKey("distance", c) && c == Column::Distance);
    CHECK(broadcast::columnFromKey("snr", c) && c == Column::Snr);
    CHECK(!broadcast::columnFromKey("nope", c));
    CHECK_EQ(broadcast::defaultColumns().size(), (size_t)broadcast::COLUMN_COUNT);

    // SNR: none until a host since Data2G PR #98 reports one.
    CHECK(!list.weakestPath(t));
    CHECK_EQ(list.cell(*w1aw, Column::Snr, t), std::string());
    list.heard("W1AW", t, 9.0);
    list.heard("W1AW", t, 7.0);
    list.heard("KC2G", t, -4.04);
    list.heard("JA1XYZ", t);                    // an older host: no SNR
    CHECK_EQ(list.cell(*w1aw, Column::Snr, t), std::string("+7.0 dB"));
    CHECK_EQ(list.cell(*list.find("KC2G"), Column::Snr, t), std::string("\xe2\x88\x92" "4.0 dB"));
    s = list.sorted(Column::Snr, true, t);
    CHECK_EQ(s[0]->call, std::string("KC2G"));
    CHECK_EQ(s[1]->call, std::string("W1AW"));
    CHECK_EQ(s[2]->call, std::string("JA1XYZ"));  // no SNR last either way
    s = list.sorted(Column::Snr, false, t);
    CHECK_EQ(s[0]->call, std::string("W1AW"));
    CHECK_EQ(s[2]->call, std::string("JA1XYZ"));

    // Auto's path: the weakest station heard lately, by its mean.
    std::optional<broadcast::PathReport> path = list.weakestPath(t);
    CHECK(path && path->call == "KC2G" && path->bursts == 1);
    t += 10 * 60000;
    list.heard("W1AW", t, -8.0);                // W1AW's mean: (9 + 7 - 8) / 3
    path = list.weakestPath(t);
    CHECK(path && path->call == "KC2G");
    t += 6 * 60000;                             // KC2G's report is 16 min old
    path = list.weakestPath(t);
    CHECK(path && path->call == "W1AW" && path->bursts == 3 && std::fabs(path->dB - 8.0 / 3.0) < 1e-9);
    t += broadcast::PATH_WINDOW_MS;
    CHECK(!list.weakestPath(t));
    // Heard again after so long, the old bursts are forgotten.
    list.heard("W1AW", t, 2.0);
    path = list.weakestPath(t);
    CHECK(path && path->bursts == 1 && path->dB == 2.0);
}

void testReconnect()
{
    link::Reconnect r;
    CHECK(!r.waiting());
    r.failed(0);
    CHECK(r.waiting() && !r.due(1999) && r.due(2000));
    CHECK_EQ(r.secondsLeft(0), 2);
    CHECK_EQ(r.secondsLeft(1001), 1);
    r.trying();
    r.failed(2000);
    CHECK_EQ(r.secondsLeft(2000), 4);
    r.failed(0);
    r.failed(0);
    r.failed(0);
    r.failed(0);
    CHECK_EQ(r.secondsLeft(0), 30);
    CHECK_EQ(r.failures(), 6);
    r.connected();
    CHECK(!r.waiting() && r.failures() == 0);
}

} // namespace

int main()
{
    testAddressee();
    testPhrasesTyped();
    testMessageBothWays();
    testAddressedAndAcknowledged();
    testLongMessageSplits();
    testPauseCancelResend();
    testDuplicateFrame();
    testHistory();
    testStations();
    testReconnect();
    return ragtime_test::finish();
}
