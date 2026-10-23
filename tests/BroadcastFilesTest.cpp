// Broadcast files, seals and repair: several stations, each a HostLink,
// BroadcastChat, Seals and BroadcastFiles, on an air in memory that
// answers the command lines and carries every ACKMODE frame to every
// other station, except what a test makes a listener lose. No sockets and
// a simulated clock, so it runs everywhere the core builds.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using broadcast::BroadcastChat;
using broadcast::BroadcastFile;
using broadcast::BroadcastFiles;
using broadcast::Seals;
using link::HostLink;
using FileState = BroadcastFile::State;

namespace
{

uint64_t now = 1792627200000; // 2026-10-22 00:00 UTC

constexpr int GROUP_PORT = 2;
const char* const MODES = "MODE qpsk-r1/3 1200 76 64 1.78 74.36\r"
                          "MODE fsk16r25-r1/3 500 38 9 4.98 89.22\r"
                          "MODE w48-16qam-r1/2 2400 236 64 1.21 37.50\r"
                          "OK\r";

struct Station
{
    Station(const std::string& call, uint32_t seed)
        : link({call, true, false}), chat(link), seals(link, chat), files(link, chat, seals)
    {
        BroadcastChat::Settings s;
        s.callsign = call;
        s.software = "Ragtime test";
        chat.setSettings(s, 0);
        seals.setChain(call, Seals::Chain());
        seals.setRandomSeed(seed);
        // Most of these tests repair chat, so they seal it; the default is
        // off (testChatUnsealed).
        seals.setSealText(true);
        seals.setMode("qpsk-r1/3");
        chat.onItems([this](const std::vector<protocol::Record>& items, uint64_t burst, const std::string& mode,
                            uint64_t at) { seals.going(items, burst, mode, at); });
        link.commandUp(now);
    }

    const std::string& call() const { return chat.settings().callsign; }

    void events()
    {
        for (const HostLink::Event& e : link.takeEvents())
        {
            std::vector<protocol::Record> records = chat.onEvent(e, now);
            seals.onEvent(e, now);
            files.onEvent(e, now);
            for (const protocol::Record& r : records)
            {
                seals.onRecord(r, now);
                files.onRecord(r, now);
                heard.push_back(r);
            }
        }
    }

    void tick()
    {
        link.tick(now);
        seals.tick(now);
        files.tick(now);
        events();
    }

    size_t heardFrom(const std::string& from, protocol::RecordType type) const
    {
        size_t n = 0;
        for (const protocol::Record& r : heard)
        {
            if (r.header.callsign == from && r.type() == (uint8_t)type) n++;
        }
        return n;
    }

    size_t textsFrom(const std::string& from) const
    {
        size_t n = 0;
        for (const broadcast::Message& m : chat.messages())
        {
            if (m.from == from && m.file == 0) n++;
        }
        return n;
    }

    HostLink link;
    BroadcastChat chat;
    Seals seals;
    BroadcastFiles files;
    link::KissDecoder kiss;
    std::vector<protocol::Record> heard;
    // Decides whether this station hears a record (true: lost), and may
    // spoil it.
    std::function<bool(protocol::Record&)> lose;
};

struct Air
{
    std::vector<std::unique_ptr<Station>> stations;
    size_t requestsOnAir = 0;
    size_t framesOnAir = 0;
    std::vector<uint64_t> sealsOnAir;   // when each Seal went

    Station& add(const std::string& call, uint32_t seed)
    {
        stations.push_back(std::make_unique<Station>(call, seed));
        return *stations.back();
    }

    void step()
    {
        for (auto& sp : stations)
        {
            Station& s = *sp;
            std::string lines = s.link.takeCommandOutput();
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
                    replies += MODES;
                else
                    replies += "OK\r";
            }
            if (!replies.empty()) s.link.commandBytes(replies.data(), replies.size(), now);

            std::vector<uint8_t> out = s.link.takeKissOutput();
            std::vector<link::KissFrame> frames;
            s.kiss.feed(out.data(), out.size(), frames);
            if (frames.empty()) continue;
            std::string ptt = "PTT ON\rMODE qpsk-r1/3\r";
            s.link.commandBytes(ptt.data(), ptt.size(), now);
            for (auto& op : stations)
            {
                if (op.get() == &s) continue;
                std::string from = "BCAST " + std::to_string(GROUP_PORT) + " HEARD " + s.call() + "\r";
                op->link.commandBytes(from.data(), from.size(), now);
            }
            for (const link::KissFrame& f : frames)
            {
                if (f.command != link::KISS_ACKMODE || f.payload.size() < 2) continue;
                std::vector<uint8_t> data(f.payload.begin() + 2, f.payload.end());
                CHECK(data.size() <= protocol::MAX_BROADCAST_FRAME);
                CHECK(!data.empty() && data[0] == protocol::FRAME_BYTE);
                framesOnAir++;
                std::vector<protocol::Record> records;
                protocol::decodeFrame(data, records);
                for (const protocol::Record& r : records)
                {
                    if (r.type() == (uint8_t)protocol::RecordType::RepairRequest) requestsOnAir++;
                    if (r.type() == (uint8_t)protocol::RecordType::Seal) sealsOnAir.push_back(now);
                }
                for (auto& op : stations)
                {
                    if (op.get() == &s) continue;
                    std::vector<protocol::Record> kept;
                    for (protocol::Record r : records)
                    {
                        if (op->lose && op->lose(r)) continue;
                        kept.push_back(std::move(r));
                    }
                    if (kept.empty()) continue;
                    std::vector<uint8_t> heard = protocol::encodeFrame(kept);
                    std::vector<uint8_t> kiss = link::kissEncode(GROUP_PORT, heard);
                    op->link.kissBytes(kiss.data(), kiss.size(), now);
                }
                uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
                std::vector<uint8_t> ack = link::kissEncodeAckMode(GROUP_PORT, tag, {});
                s.link.kissBytes(ack.data(), ack.size(), now);
            }
            std::string off = "PTT OFF\r";
            s.link.commandBytes(off.data(), off.size(), now);
        }
        for (auto& s : stations) s->events();
    }

    // The clock moves on by ms, a step at a time.
    void run(uint64_t ms, uint64_t stepMs = 250)
    {
        for (uint64_t t = 0; t < ms; t += stepMs)
        {
            now += stepMs;
            step();
            for (auto& s : stations) s->tick();
            step();
        }
    }
};

bool isType(const protocol::Record& r, protocol::RecordType type)
{
    return r.type() == (uint8_t)type;
}

// A file of n bytes of a pattern, in a scratch folder.
std::filesystem::path scratch()
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-broadcast-files-test";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string makeFile(const std::string& name, size_t n, uint32_t seed = 1)
{
    std::filesystem::path p = scratch() / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++)
    {
        x = x * 1103515245u + 12345u;
        out.put((char)(x >> 16));
    }
    return p.string();
}

std::vector<uint8_t> readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::filesystem::path freshFolder(const std::string& name)
{
    std::filesystem::path dir = scratch() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// The file heard from a station; id 0 when there is none.
BroadcastFile fileFrom(const Station& s, const std::string& from)
{
    for (const BroadcastFile& f : s.files.files(now))
    {
        if (!f.outgoing && f.from == from) return f;
    }
    return BroadcastFile();
}

//-------------------------------------------------------------------------

void testAirtime()
{
    link::ModeInfo fsk = broadcast::findMode({}, "fsk16r25-r1/3");
    link::ModeInfo qpsk = broadcast::findMode({}, "qpsk-r1/3");
    CHECK(fsk.valid());
    CHECK(qpsk.valid());
    CHECK(!broadcast::findMode({}, "no-such-mode").valid());
    CHECK(broadcast::findMode({}, "", "qpsk-r1/3").valid());

    // docs/PLAN.md's table: fsk16r25-r1/3 about 16 s a one-codeword
    // burst and a 52 s window; qpsk-r1/3 about 3 s and 13 s.
    CHECK(broadcast::oneCodewordSeconds(fsk) > 15.0 && broadcast::oneCodewordSeconds(fsk) < 16.5);
    uint64_t wf = broadcast::listeningWindowMs(fsk);
    CHECK(wf > 49000 && wf < 54000);
    uint64_t wq = broadcast::listeningWindowMs(qpsk);
    CHECK(wq > 12000 && wq < 14000);

    // Pieces fill three FSK codewords, less Data2G's length.
    CHECK_EQ(broadcast::pieceFrameBytes(fsk), 3 * 38 - 2);
    int bytes = broadcast::pieceBytesFor(fsk, "AG7EW");
    protocol::Record r;
    r.header.callsign = "AG7EW";
    protocol::FilePiece p;
    p.bytes.assign((size_t)bytes, 1);
    r.body = p;
    CHECK_EQ((int)protocol::encodeFrame({r}).size(), broadcast::pieceFrameBytes(fsk));
    CHECK(broadcast::pieceBytesFor(qpsk, "AG7EW") > bytes);
    CHECK((int)protocol::encodeFrame({r}).size() <= (int)protocol::MAX_BROADCAST_FRAME);

    // One frame a burst in FSK; several in OFDM.
    std::vector<std::vector<uint8_t>> frames(5, std::vector<uint8_t>(100, 0xA0));
    CHECK_EQ(broadcast::packBursts(frames, fsk).size(), (size_t)5);
    CHECK(broadcast::packBursts(frames, qpsk).size() < 5);

    // Records share frames while they fit.
    std::vector<protocol::Record> small;
    for (int i = 0; i < 30; i++)
    {
        protocol::Record t;
        t.header.callsign = "AG7EW";
        t.header.seq = (uint32_t)i + 1;
        protocol::Text text;
        text.text = "hello there";
        t.body = text;
        small.push_back(t);
    }
    auto packed = broadcast::packFrames(small);
    CHECK(packed.size() > 1 && packed.size() < 10);
    for (const auto& f : packed) CHECK(f.size() <= protocol::MAX_BROADCAST_FRAME);

    // The estimate box.
    broadcast::FileEstimate e = broadcast::estimateFile(7000, "FRED.TXT", "AG7EW", qpsk);
    CHECK(e.pieces > 0 && e.pieces * e.pieceBytes >= 7000);
    CHECK(e.airSeconds > 0.0 && e.wallSeconds >= e.airSeconds);
    CHECK_EQ(e.repairsMs, broadcast::MAX_OPEN_MS);
    broadcast::FileEstimate slow = broadcast::estimateFile(7000, "FRED.TXT", "AG7EW", fsk);
    CHECK(slow.airSeconds > e.airSeconds * 3);
}

// Three texts, then quiet: one seal lists them by their record digests.
void testSealAfterQuiet()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(10000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    air.run(Seals::QUIET_MS);
    CHECK_EQ(a.seals.sealsSent(), (size_t)1);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)1);

    std::vector<protocol::Record> texts;
    const protocol::Seal* seal = nullptr;
    for (const protocol::Record& r : b.heard)
    {
        if (isType(r, protocol::RecordType::Text)) texts.push_back(r);
        if (const protocol::Seal* s = r.as<protocol::Seal>()) seal = s;
    }
    CHECK(seal != nullptr);
    if (seal != nullptr && texts.size() == 3)
    {
        CHECK_EQ(seal->number, (uint16_t)1);
        CHECK(seal->previous == protocol::Digest{});
        CHECK_EQ(seal->firstItem, texts[0].header.seq);
        CHECK_EQ(seal->items.size(), (size_t)3);
        for (size_t i = 0; i < 3 && i < seal->items.size(); i++)
            CHECK(seal->items[i] == protocol::recordDigest(texts[i]));
    }
    air.run(60000);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
    CHECK_EQ(a.seals.rounds(), (size_t)0);

    // The chain: the next seal names this one.
    Seals::Chain chain = a.seals.chain();
    CHECK_EQ(chain.number, (uint16_t)1);
    a.chat.send("four", "", now);
    air.run(Seals::QUIET_MS + 5000);
    const protocol::Seal* second = nullptr;
    for (const protocol::Record& r : b.heard)
    {
        if (const protocol::Seal* s = r.as<protocol::Seal>()) second = s;
    }
    CHECK(second != nullptr && second->number == 2 && second->previous == chain.digest);
}

// A listener that missed a text asks for it after the seal and gets it.
void testRepairText()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    bool lost = false;
    b.lose = [&](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two" && !lost)
        {
            lost = true;
            return true;
        }
        return false;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    a.chat.send("three", "", now);
    air.run(5000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)3);
    CHECK_EQ(b.seals.requestsSent(), (size_t)1);
    CHECK_EQ(a.seals.requestsHeard(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
    // The resend ended with the seal again; nothing more is lacked, so
    // the next window is quiet and the repairs end.
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)2);
    air.run(120000);
    CHECK_EQ(air.requestsOnAir, (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// Two listeners miss the same text: one asks, the other hears it and
// keeps quiet.
void testSuppression()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 7);
    Station& c = air.add("W1AW", 99);
    uint64_t start = now;
    auto loseTwo = [start](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        return t != nullptr && t->text == "two" && now < start + 30000;
    };
    b.lose = loseTwo;
    c.lose = loseTwo;
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(c.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(air.requestsOnAir, (size_t)1);
    CHECK_EQ(b.seals.requestsSent() + c.seals.requestsSent(), (size_t)1);
    CHECK_EQ(b.seals.requestsHeld() + c.seals.requestsHeld(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// A station that tunes in and hears only the seal doesn't ask for the
// over it missed.
void testLateJoiner()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.lose = [](protocol::Record& r) { return r.as<protocol::Text>() != nullptr; };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)1);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
}

// A listener that missed a seal (and a text under it) hears the next
// seal name it, asks for the seal, then for the text.
void testMissedSeal()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    uint64_t start = now;
    b.lose = [start](protocol::Record& r) {
        const protocol::Text* t = r.as<protocol::Text>();
        if (t != nullptr && t->text == "two") return true;
        const protocol::Seal* s = r.as<protocol::Seal>();
        return s != nullptr && s->number == 1 && now < start + 200000;
    };
    air.run(2000);
    a.chat.send("one", "", now);
    a.chat.send("two", "", now);
    air.run(Seals::QUIET_MS + 30000);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
    // A later over.
    now = std::max(now, start + 200000);
    a.chat.send("three", "", now);
    air.run(Seals::QUIET_MS + 120000);
    CHECK(b.heardFrom("AG7EW", protocol::RecordType::Seal) >= 2);
    CHECK(b.seals.requestsSent() >= 1);
    // "two" itself is lost each time it goes, so it is asked for until the
    // listener gives up; the seal came, which is what this checks.
    bool sealOne = false;
    for (const protocol::Record& r : b.heard)
    {
        const protocol::Seal* s = r.as<protocol::Seal>();
        if (s != nullptr && s->number == 1) sealOne = true;
    }
    CHECK(sealOne);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
}

// A file, received automatically by one station, on request by another,
// and repaired for a third that lost pieces.
void testFiles()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    Station& c = air.add("W1AW", 3);
    Station& d = air.add("G4XYZ", 4);
    std::filesystem::path bDir = freshFolder("b");
    std::filesystem::path cDir = freshFolder("c");
    std::filesystem::path dDir = freshFolder("d");
    b.files.setAutoReceive(bDir.string());
    d.files.setAutoReceive(dDir.string());
    // Each listed piece is lost the first time it goes.
    auto losePieces = [](std::vector<uint16_t> which) {
        auto lost = std::make_shared<std::set<uint16_t>>();
        return [which, lost](protocol::Record& r) {
            const protocol::FilePiece* p = r.as<protocol::FilePiece>();
            if (p == nullptr || std::find(which.begin(), which.end(), p->index) == which.end()) return false;
            return lost->insert(p->index).second;
        };
    };
    d.lose = losePieces({3, 7});
    c.lose = losePieces({5});
    air.run(2000);

    std::string path = makeFile("fred.txt", 3000);
    std::string error;
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
    CHECK(id != 0);
    CHECK(error.empty());
    std::vector<files::FileRow> rows = a.files.rows(now);
    CHECK_EQ(rows.size(), (size_t)1);
    if (!rows.empty())
    {
        CHECK(rows[0].via == files::Via::Broadcast);
        CHECK_EQ(rows[0].peer, std::string(files::BROADCAST_PEER));
        CHECK(rows[0].outgoing);
        CHECK(rows[0].look == files::FileRow::Look::Moving);
    }

    air.run(60000);
    std::vector<uint8_t> want = readAll(path);
    // B had every piece and saved it at once, before any seal.
    BroadcastFile fb = fileFrom(b, "AG7EW");
    CHECK(fb.id != 0 && fb.state == FileState::Saved);
    if (fb.id != 0) CHECK(readAll(fb.path) == want);
    // C waits for the operator.
    BroadcastFile fc = fileFrom(c, "AG7EW");
    CHECK(fc.id != 0 && fc.state == FileState::Heard);
    // D lost pieces 3 and 7 and asked for them after the seal.
    BroadcastFile fd = fileFrom(d, "AG7EW");
    CHECK(fd.id != 0 && fd.state == FileState::Saved);
    if (fd.id != 0) CHECK(readAll(fd.path) == want);
    CHECK_EQ(d.seals.requestsSent(), (size_t)1);
    CHECK_EQ(c.seals.requestsSent(), (size_t)0);
    // The share is open for repairs.
    rows = a.files.rows(now);
    CHECK_EQ(rows.size(), (size_t)1);
    if (!rows.empty())
    {
        CHECK(rows[0].look == files::FileRow::Look::Repairing);
        CHECK(rows[0].expiresAtMs > now);
    }

    // C presses Receive late: its request opens a window, and the piece
    // it lacked comes again.
    size_t rounds = a.seals.rounds();
    if (fc.id != 0) CHECK(c.files.receive(fc.id, (cDir / "fred-c.txt").string(), now, error));
    air.run(60000);
    fc = fileFrom(c, "AG7EW");
    CHECK(fc.id != 0 && fc.state == FileState::Saved);
    if (fc.id != 0) CHECK_EQ(fc.path, (cDir / "fred-c.txt").string());
    CHECK(readAll((cDir / "fred-c.txt").string()) == want);
    CHECK_EQ(a.seals.rounds(), rounds + 1);

    // The Files tab's log.
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty())
    {
        CHECK(done[0].result == files::FinishedFile::Result::Saved);
        CHECK_EQ(done[0].peer, std::string("AG7EW"));
        CHECK(!done[0].outgoing);
    }

    // Closing repairs ends the share: Sent.
    CHECK(a.files.closeRepairs(id));
    done = a.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty()) CHECK(done[0].result == files::FinishedFile::Result::Sent);
    CHECK(a.files.rows(now).empty());
}

// The share's sensitivity window, and the operator's hand on it.
void testOpenWindow()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.add("KC2G", 2);
    air.run(2000);
    std::string error;
    uint64_t id = a.files.send(makeFile("small.bin", 500), "qpsk-r1/3", now, error);
    CHECK(id != 0);
    air.run(30000);
    BroadcastFile f;
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Repairing);
    Seals::SealView seal = a.seals.sealOf(f.seq, now);
    CHECK(seal.sealed && seal.open);
    // The newest seal stays open for two hours at most.
    CHECK(seal.openUntilMs > now + broadcast::MAX_OPEN_MS - 60000);
    // Kept open longer than that.
    uint64_t until = seal.openUntilMs + 30 * 60 * 1000;
    CHECK(a.files.keepOpen(id, until));
    air.run(broadcast::MAX_OPEN_MS, 60000);
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Repairing);
    CHECK(f.keptOpen);
    CHECK_EQ(f.repairsUntilMs, until);
    air.run(31 * 60 * 1000, 60000);
    CHECK(a.files.find(id, now, f));
    CHECK(f.state == FileState::Sent);
}

// A spoiled piece fails the digest: everything is asked for again, once.
void testDigestMismatch()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("spoiled");
    b.files.setAutoReceive(dir.string());
    bool spoiled = false;
    b.lose = [&](protocol::Record& r) {
        protocol::FilePiece* p = r.as<protocol::FilePiece>();
        if (p != nullptr && p->index == 1 && !spoiled)
        {
            spoiled = true;
            p->bytes[0] ^= 0xFF;
        }
        return false;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("spoil.bin", 1200, 9);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(120000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
}

// The announce lost, and only the pieces heard: they show the listener was
// there, so it asks for the announce and then for the rest.
void testLostAnnounce()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("unannounced");
    b.files.setAutoReceive(dir.string());
    bool lost = false;
    b.lose = [&lost](protocol::Record& r) {
        if (r.as<protocol::FileAnnounce>() == nullptr || lost) return false;
        lost = true;
        return true;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("unannounced.bin", 1500, 4);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    air.run(180000);
    CHECK(lost);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
}

// The file's seal lost, or the request for its repair: the seal goes
// again after a quiet window, and the listener asks again.
void testLostSealOrRequest()
{
    for (int lostRequest = 0; lostRequest < 2; lostRequest++)
    {
        Air air;
        Station& a = air.add("AG7EW", 1);
        Station& b = air.add("KC2G", 2);
        std::filesystem::path dir = freshFolder(lostRequest ? "lostrequest" : "lostseal");
        b.files.setAutoReceive(dir.string());
        bool sealLost = false, pieceLost = false, requestLost = false;
        b.lose = [&](protocol::Record& r) {
            const protocol::FilePiece* p = r.as<protocol::FilePiece>();
            if (p != nullptr && p->index == 2 && !pieceLost) return pieceLost = true;
            if (!lostRequest && r.as<protocol::Seal>() != nullptr && !sealLost) return sealLost = true;
            return false;
        };
        a.lose = [&](protocol::Record& r) {
            if (lostRequest && r.as<protocol::RepairRequest>() != nullptr && !requestLost) return requestLost = true;
            return false;
        };
        air.run(2000);
        std::string error;
        std::string path = makeFile("flushed.bin", 1500, 6);
        CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
        air.run(240000);
        CHECK(pieceLost && (lostRequest ? requestLost : sealLost));
        BroadcastFile f = fileFrom(b, "AG7EW");
        CHECK(f.id != 0 && f.state == FileState::Saved);
        if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
        CHECK_EQ(a.seals.rounds(), (size_t)1);
    }
}

// The sender stops a share part way: what went is sealed, and a listener
// that asks for the rest on the group gets all of it there, with nothing
// for the sender's operator to do. Closing repairs ends it as Cancelled.
void testStoppedShare()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("stopped");
    b.files.setAutoReceive(dir.string());
    air.run(2000);
    std::string error;
    std::string path = makeFile("stopped.bin", 2500, 8);
    uint64_t id = a.files.send(path, "qpsk-r1/3", now, error);
    CHECK(id != 0);
    BroadcastFile mine;
    for (int i = 0; i < 2000 && a.files.find(id, now, mine) && mine.held < 3; i++) air.run(250);
    CHECK(mine.held >= 3 && mine.held < mine.pieces);
    CHECK(a.files.cancel(id));
    CHECK(a.files.find(id, now, mine));
    CHECK(mine.state == FileState::Repairing && mine.stopped);
    air.run(240000);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    CHECK(b.seals.requestsSent() >= 1);
    CHECK(a.files.find(id, now, mine) && mine.state == FileState::Repairing);
    CHECK(a.files.closeRepairs(id));
    CHECK(a.files.find(id, now, mine) && mine.state == FileState::Cancelled);
    std::vector<files::FinishedFile> done = a.files.takeFinished();
    CHECK(!done.empty() && done.back().result == files::FinishedFile::Result::Cancelled);

    // Stopped before anything went: it just ends.
    uint64_t second = a.files.send(makeFile("never.bin", 2500, 9), "qpsk-r1/3", now, error);
    CHECK(a.files.cancel(second));
    CHECK(a.files.find(second, now, mine) && mine.state == FileState::Cancelled);
}

// The listener stops asking: the file goes back to Incomplete with what
// came kept, no requests go, and Ask again carries on to Saved. Reject
// (ignore) throws it away and is logged as Declined.
void testStopAndAskAgain()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("askagain");
    b.files.setAutoReceive(dir.string());
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index >= 4;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("askagain.bin", 2500, 10);
    CHECK(a.files.send(path, "qpsk-r1/3", now, error) != 0);
    // The first pass, with B hearing only its first pieces.
    for (int i = 0; i < 4000; i++)
    {
        BroadcastFile f = fileFrom(b, "AG7EW");
        if (f.id != 0 && f.held >= 4) break;
        air.run(250);
    }
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Receiving);
    CHECK(b.files.stop(f.id, now));
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Incomplete && f.stopped && f.held == 4);
    size_t asked = b.seals.requestsSent();
    air.run(180000);
    CHECK_EQ(b.seals.requestsSent(), asked);
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Incomplete);
    CHECK(b.files.takeFinished().empty());

    // Ask again: it hears the rest now.
    b.lose = nullptr;
    CHECK(b.files.receive(f.id, "", now, error));
    air.run(180000);
    f = fileFrom(b, "AG7EW");
    CHECK(f.state == FileState::Saved);
    if (f.state == FileState::Saved) CHECK(readAll(f.path) == readAll(path));

    // Reject: thrown away, logged as Declined.
    b.files.takeFinished();
    CHECK(a.files.send(makeFile("reject.bin", 1200, 11), "qpsk-r1/3", now, error) != 0);
    air.run(3000);
    BroadcastFile r;
    for (const BroadcastFile& x : b.files.files(now))
    {
        if (!x.outgoing && x.name == "reject.bin") r = x;
    }
    CHECK(r.id != 0 && b.files.ignore(r.id));
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK(!done.empty() && done.back().result == files::FinishedFile::Result::Declined);
}

// Many pieces lost: the request is cut to fit a frame, and the rest come
// in later rounds.
void testManyLost()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    std::filesystem::path dir = freshFolder("many");
    b.files.setAutoReceive(dir.string());
    std::set<uint16_t> lost;
    b.lose = [&lost](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index % 2 == 0 && lost.insert(p->index).second;
    };
    air.run(2000);
    std::string error;
    std::string path = makeFile("big.bin", 60000, 5);
    CHECK(a.files.send(path, "w48-16qam-r1/2", now, error) != 0);
    air.run(15 * 60 * 1000, 500);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Saved);
    if (f.id != 0) CHECK(readAll(f.path) == readAll(path));
    // 135 pieces lost, every other one: one request's bitmap holds them.
    CHECK_EQ(b.seals.requestsSent(), (size_t)1);
    CHECK_EQ(a.seals.rounds(), (size_t)1);
}

// What can't go.
void testRefused()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    air.run(2000);
    std::string error;
    CHECK_EQ(a.files.send(makeFile("too-big.bin", 300 * 1024 + 1), "qpsk-r1/3", now, error), (uint64_t)0);
    CHECK(error.find("300 KB") != std::string::npos);
    CHECK_EQ(a.files.send((scratch() / "not-there.bin").string(), "qpsk-r1/3", now, error), (uint64_t)0);
    CHECK_EQ(a.files.send(makeFile("empty.bin", 0), "qpsk-r1/3", now, error), (uint64_t)0);
    // 300 KB in the slowest gear is far over two hours on the air.
    CHECK_EQ(a.files.send(makeFile("slow.bin", 300 * 1024), "fsk16r25-r1/3", now, error), (uint64_t)0);
    CHECK(error.find("two hours") != std::string::npos);
}

// Ignore throws away what came, and asks for nothing.
void testIgnore()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    b.lose = [](protocol::Record& r) {
        const protocol::FilePiece* p = r.as<protocol::FilePiece>();
        return p != nullptr && p->index == 0;
    };
    air.run(2000);
    std::string error;
    CHECK(a.files.send(makeFile("ignore.bin", 2000), "qpsk-r1/3", now, error) != 0);
    air.run(5000);
    std::vector<BroadcastFiles::Event> events = b.files.takeEvents();
    bool announced = false;
    for (const auto& e : events) announced = announced || e.type == BroadcastFiles::Event::Type::Announced;
    CHECK(announced);
    BroadcastFile f = fileFrom(b, "AG7EW");
    CHECK(f.id != 0 && f.state == FileState::Heard);
    if (f.id != 0) CHECK(b.files.ignore(f.id));
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.seals.requestsSent(), (size_t)0);
    std::vector<files::FinishedFile> done = b.files.takeFinished();
    CHECK_EQ(done.size(), (size_t)1);
    if (!done.empty()) CHECK(done[0].result == files::FinishedFile::Result::Declined);
}

} // namespace

// Chat seals are off by default (Jeff, 2026-10-22: on a poor link a seal
// takes 30 to 60 s, and plain chat must never pay for it unasked): a chat
// over goes with no seal, and a file is still sealed, listing only its
// announce.
void testChatUnsealed()
{
    Air air;
    Station& a = air.add("AG7EW", 1);
    Station& b = air.add("KC2G", 2);
    a.seals.setSealText(false);
    air.run(2000);
    a.chat.send("CQ CQ", "", now);
    a.chat.send("de AG7EW", "", now);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(b.textsFrom("AG7EW"), (size_t)2);
    CHECK_EQ(a.seals.sealsSent(), (size_t)0);
    CHECK_EQ(b.heardFrom("AG7EW", protocol::RecordType::Seal), (size_t)0);
    CHECK_EQ(air.requestsOnAir, (size_t)0);

    // A file between two lines of chat: its seal lists the announce alone.
    a.chat.send("a file for the net", "", now);
    std::string error;
    CHECK(a.files.send(makeFile("net.txt", 600), "qpsk-r1/3", now, error) != 0);
    a.chat.send("73", "", now);
    air.run(Seals::QUIET_MS + 60000);
    const protocol::Seal* seal = nullptr;
    uint32_t announce = 0;
    for (const protocol::Record& r : b.heard)
    {
        if (const protocol::Seal* s = r.as<protocol::Seal>()) seal = s;
        if (isType(r, protocol::RecordType::FileAnnounce)) announce = r.header.seq;
    }
    CHECK_EQ(a.seals.sealsSent(), (size_t)1);
    CHECK(seal != nullptr && announce != 0);
    if (seal != nullptr)
    {
        CHECK_EQ(seal->firstItem, announce);
        CHECK_EQ(seal->items.size(), (size_t)1);
    }
    BroadcastFile file = fileFrom(b, "AG7EW");
    CHECK(file.id != 0);

    // Chat between two files leaves a gap in the numbers: two seals, made
    // together and keyed together, not one keying each.
    air.sealsOnAir.clear();
    CHECK(a.files.send(makeFile("one.txt", 300), "qpsk-r1/3", now, error) != 0);
    a.chat.send("and another", "", now);
    CHECK(a.files.send(makeFile("two.txt", 300), "qpsk-r1/3", now, error) != 0);
    air.run(Seals::QUIET_MS + 60000);
    CHECK_EQ(a.seals.sealsSent(), (size_t)3);
    // Each goes three times (a file's seal is repeated), always as a pair.
    CHECK_EQ(air.sealsOnAir.size(), (size_t)6);
    for (size_t i = 0; i + 1 < air.sealsOnAir.size(); i += 2) CHECK_EQ(air.sealsOnAir[i], air.sealsOnAir[i + 1]);
    CHECK_EQ(air.requestsOnAir, (size_t)0);
}

int main()
{
    testAirtime();
    testSealAfterQuiet();
    testRepairText();
    testSuppression();
    testLateJoiner();
    testMissedSeal();
    testFiles();
    testOpenWindow();
    testDigestMismatch();
    testLostAnnounce();
    testLostSealOrRequest();
    testStoppedShare();
    testStopAndAskAgain();
    testManyLost();
    testRefused();
    testIgnore();
    testChatUnsealed();
    std::filesystem::remove_all(scratch());
    return ragtime_test::finish();
}
