// The session file engine on its own: two engines whose records are
// carried between them by hand, standing in for a session, with the files
// in a scratch folder. Ported from Glissando's Data2GFileTransferTest and
// moved onto Ragtime's records.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"

using namespace ragtime;
using namespace ragtime::files;
using State = FileTransfer::State;

namespace
{

namespace fs = std::filesystem;

constexpr uint64_t NOW = 1792627200000; // 2026-10-22 00:00 UTC, in ms

// A folder of its own for each test, removed afterwards.
struct Scratch
{
    explicit Scratch(const std::string& name)
    {
        std::random_device random;
        dir = fs::temp_directory_path() / ("ragtime-file-test-" + name + "-" + std::to_string(random()));
        fs::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string path(const std::string& name) const { return utf8FromPath(dir / pathFromUtf8(name)); }

    std::string write(const std::string& name, size_t size)
    {
        std::string content(size, '\0');
        for (size_t i = 0; i < size; i++) content[i] = (char)((i * 7 + i / 251) & 0xFF);
        std::ofstream out(dir / pathFromUtf8(name), std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
        return path(name);
    }

    fs::path dir;
};

std::string readFile(const std::string& path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool exists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(pathFromUtf8(path), ec);
}

FileTransfer transferOf(const SessionFiles& engine, uint64_t id)
{
    for (const FileTransfer& t : engine.transfers())
    {
        if (t.id == id) return t;
    }
    return FileTransfer();
}

// The one transfer the engine has from (or to) the far end, newest.
FileTransfer last(const SessionFiles& engine, bool outgoing)
{
    FileTransfer found;
    for (const FileTransfer& t : engine.transfers())
    {
        if (t.outgoing == outgoing) found = t;
    }
    return found;
}

// Two stations in a session. What one writes reaches the other through a
// stream reader, as it would through data2g-host, and the far end's modem
// acknowledges it at once unless told to hold.
struct Link
{
    SessionFiles a; // W1AW
    SessionFiles b; // VK3ABC
    protocol::Numbering numA{"W1AW"}, numB{"VK3ABC"};
    protocol::StreamReader toA, toB;
    uint64_t writtenByA = 0, writtenByB = 0;
    uint64_t nowMs = NOW;
    bool holdPieces = false;   // only records, no pieces
    int pieces = 0;
    uint32_t lastOfferToB = 0; // the sequence number of the latest FileOffer B heard

    Link() { newSession(); }

    // Each side opens its stream with its Hello, numbered afresh.
    void newSession()
    {
        numA = protocol::Numbering("W1AW");
        numB = protocol::Numbering("VK3ABC");
        toA.reset();
        toB.reset();
        protocol::Hello hello;
        std::vector<protocol::Record> none;
        CHECK(toB.feed(protocol::streamOpening(numA.record(hello, 0)), none));
        CHECK(toA.feed(protocol::streamOpening(numB.record(hello, 0)), none));
    }

    // One turn each: records, then a piece; then everything is delivered.
    void turn()
    {
        std::vector<uint8_t> fromA = a.takeRecords("VK3ABC", numA, nowMs);
        if (!holdPieces && a.takePiece("VK3ABC", numA, writtenByA + fromA.size(), nowMs, fromA)) pieces++;
        writtenByA += fromA.size();
        std::vector<uint8_t> fromB = b.takeRecords("W1AW", numB, nowMs);
        b.takePiece("W1AW", numB, writtenByB + fromB.size(), nowMs, fromB);
        writtenByB += fromB.size();

        deliver(fromA, toB, b, "W1AW");
        deliver(fromB, toA, a, "VK3ABC");
        a.acknowledged("VK3ABC", writtenByA);
        b.acknowledged("W1AW", writtenByB);
        a.tick(nowMs);
        b.tick(nowMs);
    }

    void deliver(const std::vector<uint8_t>& bytes, protocol::StreamReader& reader, SessionFiles& to,
                 const std::string& from)
    {
        if (bytes.empty()) return;
        std::vector<protocol::Record> records;
        // In uneven reads, as a socket would hand them over.
        for (size_t at = 0; at < bytes.size(); at += 1000)
        {
            size_t n = std::min<size_t>(1000, bytes.size() - at);
            CHECK(reader.feed(bytes.data() + at, n, records));
        }
        for (const protocol::Record& record : records)
        {
            CHECK_EQ(record.header.callsign, from);
            if (&to == &b && record.as<protocol::FileOffer>() != nullptr) lastOfferToB = record.header.seq;
            to.onRecord(from, record, nowMs);
        }
    }

    void turns(int n)
    {
        for (int i = 0; i < n; i++) turn();
    }
};

// A FileOffer of this content, as station call's offer number.
protocol::Record offerRecord(uint32_t number, const std::string& content, const std::string& name,
                             const std::string& call = "W1AW")
{
    protocol::FileOffer offer;
    offer.size = (uint32_t)content.size();
    offer.digest = protocol::digestOf((const uint8_t*)content.data(), content.size());
    offer.name = name;
    protocol::Record record;
    record.header = {call, number, (uint32_t)(NOW / 1000)};
    record.body = offer;
    return record;
}

protocol::Record dataRecord(uint32_t number, const std::string& bytes, uint32_t offset = 0,
                            const std::string& call = "W1AW")
{
    protocol::FileData data;
    data.offset = offset;
    data.bytes.assign(bytes.begin(), bytes.end());
    protocol::Record record;
    record.header = {call, number, (uint32_t)(NOW / 1000)};
    record.body = data;
    return record;
}

// The records an engine has for the station, decoded.
std::vector<protocol::Record> recordsFrom(SessionFiles& engine, const std::string& peer)
{
    protocol::Numbering numbering("VK3ABC");
    std::vector<uint8_t> stream = protocol::streamOpening(numbering.record(protocol::Hello(), 0));
    std::vector<uint8_t> bytes = engine.takeRecords(peer, numbering, NOW);
    stream.insert(stream.end(), bytes.begin(), bytes.end());
    protocol::StreamReader reader;
    std::vector<protocol::Record> records;
    CHECK(reader.feed(stream, records));
    if (!records.empty()) records.erase(records.begin()); // the Hello
    return records;
}

void testSafeNames()
{
    CHECK(safeFileName("FRED.TXT") == "FRED.TXT");
    CHECK(safeFileName("../../x") == "x");
    CHECK(safeFileName("..\\..\\windows\\system32\\evil.dll") == "evil.dll");
    CHECK(safeFileName("/etc/passwd") == "passwd");
    CHECK(safeFileName("") == "received-file");
    CHECK(safeFileName("..") == "received-file");
    CHECK(safeFileName("dir/") == "received-file");
    CHECK(safeFileName("a\x01" "b\x1F" "c\x7F.txt") == "abc.txt");
    CHECK(safeFileName("what?<is>:this|\"*.txt") == "whatisthis.txt");
    CHECK(safeFileName(".bashrc") == "bashrc");
    CHECK(safeFileName("trailing. . ") == "trailing");
    CHECK(safeFileName("CON") == "received-file");
    CHECK(safeFileName("con.txt") == "received-file.txt");
    CHECK(safeFileName("Lpt1.tar.gz") == "received-file.tar.gz");
    CHECK(safeFileName("COM10.txt") == "COM10.txt");
    CHECK(safeFileName("COM\xC2\xB9.txt") == "received-file.txt");    // COM superscript one
    CHECK(safeFileName("lpt\xC2\xB2") == "received-file");            // superscript two
    CHECK(safeFileName("COM\xC2\xB3.log") == "received-file.log");    // superscript three
    CHECK(safeFileName("COM\xC2\xB4.txt") == "COM\xC2\xB4.txt");      // an acute accent: not a port
    CHECK(safeFileName("console.txt") == "console.txt");
    CHECK(safeFileName("caf\xC3\xA9.txt") == "caf\xC3\xA9.txt");     // UTF-8 kept
    CHECK(safeFileName("bad\xFF\xC3.txt") == "bad.txt");             // not UTF-8: dropped
    CHECK(safeFileName("c1\xC2\x85" "ctl") == "c1ctl");              // NEL, a C1 control
    // Direction controls, which could show another extension than the real one
    CHECK(safeFileName("invoice\xE2\x80\xAE" "fdp.exe") == "invoicefdp.exe");      // U+202E
    CHECK(safeFileName("a\xE2\x80\x8E" "b\xE2\x80\x8F" "c\xE2\x80\xAA.txt") == "abc.txt");
    CHECK(safeFileName("x\xE2\x81\xA6" "y\xE2\x81\xA9.txt") == "xy.txt");       // isolates
    CHECK(safeFileName("it\xE2\x80\x99s.txt") == "it\xE2\x80\x99s.txt");        // a quote is kept
    std::string longName(300, 'a');
    CHECK(safeFileName(longName).size() == protocol::MAX_FILE_NAME);
    // Cut where a character ends, not in the middle of one.
    std::string accents;
    for (int i = 0; i < 150; i++) accents += "\xC3\xA9";
    std::string cut = safeFileName(accents);
    CHECK(cut.size() == protocol::MAX_FILE_NAME && cut.substr(cut.size() - 2) == "\xC3\xA9");
}

void testAFileEndToEnd()
{
    Scratch scratch("end-to-end");
    Link link;
    std::string source = scratch.write("FRED.TXT", 7000);
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", source, link.nowMs, error);
    CHECK(id != 0);
    CHECK(transferOf(link.a, id).state == State::Waiting);
    CHECK(link.a.busyWith("VK3ABC"));
    std::string peer;
    uint64_t since = 0;
    CHECK(link.a.wantsSession(peer, since) && peer == "VK3ABC" && since == link.nowMs);

    link.turn();
    CHECK(transferOf(link.a, id).state == State::Offered);
    FileTransfer offered = last(link.b, false);
    CHECK(offered.state == State::Asking && offered.name == "FRED.TXT" && offered.size == 7000 &&
          offered.peer == "W1AW");
    CHECK(link.b.busyWith("W1AW"));
    CHECK(!link.a.wantsSession(peer, since));

    std::string saveAs = scratch.path("saved.txt");
    CHECK(link.b.accept(offered.id, saveAs, error));
    CHECK(exists(saveAs + ".part"));
    link.turn(); // the Accept reaches A
    CHECK(transferOf(link.a, id).state == State::Sending && transferOf(link.a, id).done == 0);
    link.turn(); // its first piece goes
    CHECK(transferOf(link.a, id).done == 4096);
    CHECK(last(link.b, false).done == 4096 && last(link.b, false).state == State::Receiving);
    link.turns(2);
    CHECK(transferOf(link.a, id).state == State::Delivered && transferOf(link.a, id).done == 7000);
    CHECK(last(link.b, false).state == State::Saved && last(link.b, false).path == saveAs);
    CHECK(readFile(saveAs) == readFile(source));
    CHECK(!exists(saveAs + ".part"));
    CHECK(link.pieces == 2);
    CHECK(!link.a.busyWith("VK3ABC") && !link.b.busyWith("W1AW"));
}

void testAnEmptyFile()
{
    Scratch scratch("empty");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("empty.bin", 0), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("got.bin"), error));
    link.turns(2);
    CHECK(transferOf(link.a, id).state == State::Delivered);
    CHECK(last(link.b, false).state == State::Saved && exists(scratch.path("got.bin")));
    CHECK(link.pieces == 0);
}

void testDeclined()
{
    Scratch scratch("decline");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(link.b.decline(last(link.b, false).id));
    CHECK(last(link.b, false).state == State::Declined);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::Declined);
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("late.bin"), error));
}

void testTheSenderCancels()
{
    Scratch scratch("sender-cancels");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("big.bin.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(3);
    CHECK(last(link.b, false).state == State::Receiving && last(link.b, false).done == 8192);

    // A piece already written when the sender cancels arrives after it in
    // the stream would be fine; here one arrives after the Cancel is acted on.
    std::vector<uint8_t> late;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, late));
    CHECK(link.a.cancel(id));
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    link.turn();
    CHECK(last(link.b, false).state == State::CancelledThere);
    // What came is kept, for the same file sent again.
    CHECK(exists(saveAs + ".part") && !exists(saveAs) && link.b.keptCount() == 1);
    link.deliver(late, link.toB, link.b, "W1AW"); // dropped
    CHECK(last(link.b, false).state == State::CancelledThere && last(link.b, false).done == 8192);
    CHECK(exists(saveAs + ".part") && link.b.keptCount() == 1);
    std::vector<uint8_t> none;
    CHECK(!link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, none) && none.empty());
}

// The sender cancels with pieces still at the host: nothing more is
// handed over, and the host sends what it has, so the file isn't finished
// on either side. A has a part sent, counted as the far end's modem
// acknowledges it, and B keeps all that came; sent again, only the rest
// goes.
void testACancelLeavesAPartSent()
{
    Scratch scratch("part-sent");
    Link link;
    std::string error;
    std::string source = scratch.write("big.bin", 20000);
    uint64_t id = link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(transferOf(link.a, id).done == 4096);

    // Two more pieces at the host, not yet sent, when A cancels.
    std::vector<uint8_t> atHost;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, atHost));
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA + atHost.size(), link.nowMs, atHost));
    CHECK(link.a.cancel(id));
    FileTransfer cancelled = transferOf(link.a, id);
    CHECK(cancelled.state == State::Cancelled && cancelled.partKept);
    std::vector<uint8_t> none;
    CHECK(!link.a.takePiece("VK3ABC", link.numA, link.writtenByA + atHost.size(), link.nowMs, none));
    std::vector<KeptPart> sent = link.a.keptParts();
    CHECK(sent.size() == 1 && link.a.keptCount() == 0);
    if (sent.size() != 1) return;
    CHECK(sent[0].outgoing && sent[0].path == source && sent[0].part.empty());
    CHECK(sent[0].bytes == 4096 && sent[0].handed == 12288);
    CHECK_EQ(partStateText(sent[0]), std::string("Stopping, 8,192 bytes still going out"));

    // The host sends them, then the Cancel behind them.
    std::vector<uint8_t> cancel = link.a.takeRecords("VK3ABC", link.numA, link.nowMs);
    atHost.insert(atHost.end(), cancel.begin(), cancel.end());
    // Half of it acknowledged, then all.
    link.a.acknowledged("VK3ABC", link.writtenByA + atHost.size() / 2);
    sent = link.a.keptParts();
    CHECK(sent.size() == 1 && sent[0].bytes > 4096 && sent[0].bytes < 12288 && sent[0].handed == 12288);
    link.writtenByA += atHost.size();
    link.deliver(atHost, link.toB, link.b, "W1AW");
    link.a.acknowledged("VK3ABC", link.writtenByA);
    sent = link.a.keptParts();
    CHECK(sent.size() == 1 && sent[0].bytes == 12288 && sent[0].handed == 12288);
    CHECK_EQ(partStateText(sent[0]), std::string("Part sent"));
    // B kept every byte of it, and says who stopped it.
    CHECK(last(link.b, false).state == State::CancelledThere && last(link.b, false).partKept);
    std::vector<KeptPart> keptThere = link.b.keptParts();
    CHECK(keptThere.size() == 1 && keptThere[0].bytes == 12288 && !keptThere[0].outgoing);

    // Send the rest: the part sent goes with the new offer, and B carries on.
    uint64_t again = link.a.offer("VK3ABC", source, link.nowMs, error);
    CHECK(link.a.keptParts().empty());
    link.turn();
    FileTransfer offered = last(link.b, false);
    CHECK(offered.kept == 12288 && offered.keptPath == saveAs);
    CHECK(link.b.accept(offered.id, saveAs, error));
    link.turns(4);
    CHECK(transferOf(link.a, again).state == State::Delivered && transferOf(link.a, again).resumedFrom == 12288);
    CHECK(readFile(saveAs) == readFile(source));
}

// The ending a part sent waits with (as the Files tab sets it) goes to
// Completed when the part is forgotten; one the session ended under with
// none of it acknowledged ends at once.
void testAPartSentIsForgotten()
{
    Scratch scratch("part-sent-forgotten");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("got.bin"), error));
    link.turns(2);
    CHECK(link.a.cancel(id));
    std::vector<KeptPart> sent = link.a.keptParts();
    CHECK(sent.size() == 1);
    if (sent.size() != 1) return;
    FinishedFile ending;
    ending.outgoing = true;
    ending.peer = "VK3ABC";
    ending.result = FinishedFile::Result::Cancelled;
    link.a.setKeptEnding(id, ending);
    CHECK_EQ(partStateText(link.a.keptParts()[0]), std::string("Part sent, cancelled"));
    CHECK(link.a.deleteKept(sent[0].id) && link.a.keptParts().empty());
    std::vector<FinishedFile> ended = link.a.takeEnded();
    CHECK(ended.size() == 1 && ended[0].result == FinishedFile::Result::Cancelled);

    // Cancelled before any of it was acknowledged, and the session ends.
    uint64_t second = link.a.offer("VK3ABC", scratch.write("two.bin", 20000), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("two.got"), error));
    link.turn();
    std::vector<uint8_t> atHost;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, atHost));
    CHECK(transferOf(link.a, second).done == 0 && link.a.cancel(second));
    CHECK(link.a.keptParts().size() == 1 && transferOf(link.a, second).partKept);
    link.a.sessionEnded("VK3ABC", link.nowMs);
    // Not yet in Completed: the transfer goes there itself.
    CHECK(link.a.keptParts().empty() && !transferOf(link.a, second).partKept);
}

// Jeff's 3,000 that became 1,244: what has come of a piece still arriving
// is kept with the part, not thrown away.
void testAPieceStillArrivingIsKept()
{
    Scratch scratch("arriving-kept");
    Link link;
    std::string error;
    std::string source = scratch.write("big.bin", 7000);
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(last(link.b, false).done == 4096);

    // The second piece, 1,500 of its bytes in.
    std::vector<uint8_t> piece;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, piece));
    const size_t header = 1 + 2 + 4 + 3 + 4 + 4;
    std::vector<protocol::Record> records;
    CHECK(link.toB.feed(piece.data(), header + 1500, records) && records.empty());
    uint32_t number = 0, offset = 0;
    const uint8_t* data = nullptr;
    size_t partial = 0;
    CHECK(link.toB.partialFileData(number, offset, data, partial) && offset == 4096 && partial == 1500);
    link.b.arriving("W1AW", number, offset, data, partial);
    CHECK(last(link.b, false).handed == 4096 + 1280); // to the step

    CHECK(link.b.cancel(last(link.b, false).id));
    std::vector<KeptPart> kept = link.b.keptParts();
    CHECK(kept.size() == 1 && kept[0].bytes == 5596);
    CHECK(readFile(saveAs + ".part") == readFile(source).substr(0, 5596));
    // The rest of the piece comes, and is dropped.
    CHECK(link.toB.feed(piece.data() + header + 1500, piece.size() - header - 1500, records));
    for (const protocol::Record& r : records) link.b.onRecord("W1AW", r, link.nowMs);
    link.writtenByA += piece.size();
    link.turn();
    CHECK(link.b.keptParts().size() == 1 && link.b.keptParts()[0].bytes == 5596);

    // Sent again, it carries on from there, and the file is whole.
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    FileTransfer offered = last(link.b, false);
    CHECK(offered.kept == 5596);
    CHECK(link.b.accept(offered.id, saveAs, error));
    link.turns(3);
    CHECK(last(link.b, false).state == State::Saved && last(link.b, false).resumedFrom == 5596);
    CHECK(readFile(saveAs) == readFile(source));
}

void testTheReceiverCancels()
{
    Scratch scratch("receiver-cancels");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(link.b.cancel(last(link.b, false).id));
    CHECK(last(link.b, false).state == State::Cancelled);
    CHECK(exists(saveAs + ".part") && link.b.keptCount() == 1);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::CancelledThere);
}

void testAnOfferExpiresOnBothSides()
{
    Scratch scratch("expiry");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).state == State::Asking);
    link.nowMs += SessionFiles::OFFER_EXPIRY_MS - 1;
    link.turn();
    CHECK(transferOf(link.a, id).state == State::Offered);
    link.nowMs += 1;
    link.turn(); // A expires and says so
    CHECK(transferOf(link.a, id).state == State::Expired);
    link.turn();
    CHECK(last(link.b, false).state == State::Expired);
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("late.bin"), error));
    CHECK(error == "The offer has expired.");

    // The receiver's own clock runs out too, without a word from the sender.
    Link quiet;
    uint64_t second = quiet.a.offer("VK3ABC", scratch.write("y.bin", 10), quiet.nowMs, error);
    quiet.turn();
    quiet.b.tick(quiet.nowMs + SessionFiles::OFFER_EXPIRY_MS);
    CHECK(last(quiet.b, false).state == State::Expired);
    quiet.turn(); // its Cancel reaches the sender first
    CHECK(transferOf(quiet.a, second).state == State::Expired);
}

void testALostSession()
{
    Scratch scratch("lost");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    uint64_t next = link.a.offer("VK3ABC", scratch.write("next.bin", 5), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(exists(saveAs + ".part"));

    link.a.sessionEnded("VK3ABC", link.nowMs + 5);
    link.b.sessionEnded("W1AW", link.nowMs + 5);
    CHECK(transferOf(link.a, id).state == State::Failed);
    CHECK(last(link.b, false).state == State::Failed);
    CHECK(exists(saveAs + ".part") && link.b.keptCount() == 1);

    // The next file waits for the next session, from now.
    std::string peer;
    uint64_t since = 0;
    CHECK(transferOf(link.a, next).state == State::Waiting);
    CHECK(link.a.wantsSession(peer, since) && since == link.nowMs + 5);
    link.a.noSession("VK3ABC", "No answer.");
    CHECK(transferOf(link.a, next).state == State::Failed && transferOf(link.a, next).error == "No answer.");
}

// The far end's Hello says it takes no files: nothing is offered.
void testAStationThatTakesNoFiles()
{
    Scratch scratch("not-taken");
    SessionFiles a;
    std::string error;
    uint64_t id = a.offer("VK3ABC", scratch.write("x.bin", 10), NOW, error);
    a.notTaken("VK3ABC");
    CHECK(transferOf(a, id).state == State::NotTaken && !transferOf(a, id).error.empty());
    protocol::Numbering numbering("W1AW");
    CHECK(a.takeRecords("VK3ABC", numbering, NOW).empty());

    // An offer made, then the session lost: failed, with the reason.
    uint64_t second = a.offer("VK3ABC", scratch.write("y.bin", 10), NOW, error);
    CHECK(!a.takeRecords("VK3ABC", numbering, NOW).empty());
    a.sessionEnded("VK3ABC", NOW + 1000);
    CHECK(transferOf(a, second).state == State::Failed && !transferOf(a, second).error.empty());
}

// The sender cancels once every piece is written, but the receiver had
// saved it already and ignores the Cancel: its Saved, crossing the
// Cancel, makes it delivered on both sides.
void testACancelCrossingSaved()
{
    Scratch scratch("cancel-crossing-saved");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 5000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("x.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turn(); // the Accept reaches A

    // Both pieces written; B has them and has saved it.
    std::vector<uint8_t> pieces;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, pieces));
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA + pieces.size(), link.nowMs, pieces));
    link.writtenByA += pieces.size();
    link.deliver(pieces, link.toB, link.b, "W1AW");
    CHECK(last(link.b, false).state == State::Saved);
    CHECK(transferOf(link.a, id).state == State::Sending);

    CHECK(link.a.cancel(id));
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    link.turn(); // A's Cancel and B's Saved cross
    CHECK(last(link.b, false).state == State::Saved && readFile(saveAs).size() == 5000);
    CHECK(transferOf(link.a, id).state == State::Delivered && transferOf(link.a, id).done == 5000);
    // Delivered after all: no part sent is left over.
    CHECK(!transferOf(link.a, id).partKept && link.a.keptParts().empty());

    // Cancelled with only part of it written, a Saved can't come, and
    // one that did is not taken.
    uint64_t second = link.a.offer("VK3ABC", scratch.write("y.bin", 5000), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("y.got"), error));
    link.turn();
    std::vector<uint8_t> one;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, one));
    CHECK(link.a.cancel(second));
    protocol::FileSaved saved;
    saved.offer = link.lastOfferToB;
    link.a.onRecord("VK3ABC", link.numB.record(saved, 0), link.nowMs);
    CHECK(transferOf(link.a, second).state == State::Cancelled);
}

void testOneFileAtATime()
{
    Scratch scratch("queue");
    Link link;
    std::string error;
    uint64_t first = link.a.offer("VK3ABC", scratch.write("one.bin", 5000), link.nowMs, error);
    uint64_t second = link.a.offer("VK3ABC", scratch.write("two.bin", 100), link.nowMs, error);
    link.turn();
    CHECK(transferOf(link.a, first).state == State::Offered);
    CHECK(transferOf(link.a, second).state == State::Waiting);
    CHECK(link.b.transfers().size() == 1);
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("one.got"), error));
    link.turns(4);
    CHECK(transferOf(link.a, first).state == State::Delivered);
    link.turn();
    CHECK(transferOf(link.a, second).state == State::Offered);
    CHECK(last(link.b, false).name == "two.bin");
}

void testAutoAcceptNumbersAClash()
{
    Scratch scratch("auto");
    Link link;
    fs::create_directories(scratch.dir / "in");
    std::string folder = utf8FromPath(scratch.dir / "in");
    { std::ofstream(scratch.dir / "in" / "FRED.TXT") << "already here"; }
    link.b.setAutoAccept(folder, {"w1aw", "K1ABC"});
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("FRED.TXT", 3000), link.nowMs, error);
    link.turns(4);
    FileTransfer got = last(link.b, false);
    CHECK(got.autoAccepted && got.state == State::Saved);
    CHECK(got.path == utf8FromPath(scratch.dir / "in" / "FRED (2).TXT"));
    CHECK(readFile(got.path) == readFile(scratch.path("FRED.TXT")));
    CHECK(transferOf(link.a, id).state == State::Delivered);

    // Not from a station on the list: asked.
    link.b.setAutoAccept(folder, {"K1ABC"});
    link.a.offer("VK3ABC", scratch.write("other.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).state == State::Asking && !last(link.b, false).autoAccepted);
}

// A file of the operator's that happens to have the part file's name is
// neither written over nor deleted.
void testAPartFileOfTheOperatorsIsLeftAlone()
{
    Scratch scratch("own-part");
    { std::ofstream(scratch.dir / "notes.part") << "mine"; }
    SessionFiles b;
    std::string error;
    b.onRecord("W1AW", offerRecord(1, "abc", "notes"), NOW);
    CHECK(b.accept(last(b, false).id, scratch.path("notes"), error));
    CHECK(readFile(scratch.path("notes.part")) == "mine");
    CHECK(b.cancel(last(b, false).id));
    CHECK(readFile(scratch.path("notes.part")) == "mine");
    CHECK(!exists(scratch.path("notes.2.part")));

    b.onRecord("W1AW", offerRecord(2, "abc", "notes"), NOW);
    CHECK(b.accept(last(b, false).id, scratch.path("notes"), error));
    b.onRecord("W1AW", dataRecord(2, "abc"), NOW);
    CHECK(last(b, false).state == State::Saved);
    CHECK(readFile(scratch.path("notes")) == "abc");
    CHECK(readFile(scratch.path("notes.part")) == "mine");
}

// Two files can't be saved under one name at once: the second is refused
// and its offer left open.
void testTwoFilesCantBeSavedAsOne()
{
    Scratch scratch("same-name");
    SessionFiles b;
    std::string error;
    b.onRecord("W1AW", offerRecord(1, "one", "log.adi"), NOW);
    uint64_t first = last(b, false).id;
    b.onRecord("K1ABC", offerRecord(1, "two", "log.adi", "K1ABC"), NOW);
    uint64_t second = last(b, false).id;
    CHECK(b.accept(first, scratch.path("log.adi"), error));
    CHECK(!b.accept(second, scratch.path("log.adi"), error) && !error.empty());
    CHECK(transferOf(b, second).state == State::Asking);
    CHECK(b.accept(second, scratch.path("log2.adi"), error));
    b.onRecord("K1ABC", dataRecord(1, "two", 0, "K1ABC"), NOW);
    b.onRecord("W1AW", dataRecord(1, "one"), NOW);
    CHECK(readFile(scratch.path("log.adi")) == "one" && readFile(scratch.path("log2.adi")) == "two");
}

// With every numbered name taken, a file from the auto-accept list is
// asked about rather than written over one.
void testAutoAcceptNeverOverwrites()
{
    Scratch scratch("auto-full");
    { std::ofstream(scratch.dir / "beacon.txt") << "first"; }
    for (int n = 2; n < 1000; n++)
    {
        std::ofstream(scratch.dir / ("beacon (" + std::to_string(n) + ").txt"));
    }
    SessionFiles b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    b.onRecord("W1AW", offerRecord(1, "abc", "beacon.txt"), NOW);
    FileTransfer got = last(b, false);
    CHECK(got.state == State::Asking && !got.autoAccepted && !got.error.empty());
    CHECK(readFile(scratch.path("beacon.txt")) == "first");
    CHECK(!exists(scratch.path("beacon.txt.part")));
}

// A file that turns up under the auto-accepted name while the transfer
// runs is not replaced: the file gets the next free name. One the
// operator chose in the save dialog is replaced, as they agreed to.
void testAutoAcceptDoesntReplaceAFileMadeMeanwhile()
{
    Scratch scratch("auto-meanwhile");
    SessionFiles b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    b.onRecord("W1AW", offerRecord(1, "abc", "notes.txt"), NOW);
    CHECK(last(b, false).state == State::Receiving && last(b, false).path == scratch.path("notes.txt"));
    { std::ofstream(scratch.dir / "notes.txt") << "the operator's"; }
    b.onRecord("W1AW", dataRecord(1, "abc"), NOW);
    FileTransfer got = last(b, false);
    CHECK(got.state == State::Saved && got.path == scratch.path("notes (2).txt"));
    CHECK(readFile(scratch.path("notes.txt")) == "the operator's");
    CHECK(readFile(got.path) == "abc");

    // A file named for another's part file, finishing first, leaves that
    // part file alone.
    b.onRecord("W1AW", offerRecord(2, "one", "log.txt.part"), NOW);
    b.onRecord("W1AW", offerRecord(4, "two", "log.txt"), NOW);
    CHECK(last(b, false).path == scratch.path("log.txt") && exists(scratch.path("log.txt.part")));
    b.onRecord("W1AW", dataRecord(2, "one"), NOW);
    b.onRecord("W1AW", dataRecord(4, "two"), NOW);
    CHECK(readFile(scratch.path("log.txt")) == "two");
    CHECK(readFile(scratch.path("log.txt (2).part")) == "one");

    b.setAutoAccept("", {});
    { std::ofstream(scratch.dir / "chosen.txt") << "old"; }
    b.onRecord("W1AW", offerRecord(3, "new", "chosen.txt"), NOW);
    std::string error;
    CHECK(b.accept(last(b, false).id, scratch.path("chosen.txt"), error));
    b.onRecord("W1AW", dataRecord(3, "new"), NOW);
    CHECK(last(b, false).state == State::Saved && readFile(scratch.path("chosen.txt")) == "new");
}

void testAHostileNameIsSavedSafely()
{
    Scratch scratch("hostile");
    SessionFiles b;
    b.setAutoAccept(utf8FromPath(scratch.dir), {"W1AW"});
    b.onRecord("W1AW", offerRecord(7, "abc", "../../evil\x01.sh"), NOW);
    b.onRecord("W1AW", dataRecord(7, "abc"), NOW);
    FileTransfer got = last(b, false);
    CHECK(got.name == "evil.sh");
    CHECK(got.state == State::Saved && got.path == utf8FromPath(scratch.dir / "evil.sh"));
    CHECK(readFile(got.path) == "abc");

    // Nothing was written outside the folder, and nothing runs it.
    CHECK(!exists(utf8FromPath(scratch.dir.parent_path().parent_path() / "evil.sh")));
}

void testTheReceiverCantWrite()
{
    Scratch scratch("cant-write");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    CHECK(!link.b.accept(last(link.b, false).id, scratch.path("no/such/folder/x.bin"), error));
    CHECK(!error.empty());
    CHECK(last(link.b, false).state == State::Failed);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::FailedThere);
}

void testMoreThanOfferedIsRefused()
{
    Scratch scratch("too-much");
    SessionFiles b;
    b.onRecord("W1AW", offerRecord(3, "xy", "x"), NOW);
    std::string error;
    CHECK(b.accept(last(b, false).id, scratch.path("x"), error));
    b.onRecord("W1AW", dataRecord(3, "abc"), NOW);
    CHECK(last(b, false).state == State::Failed);
    CHECK(!exists(scratch.path("x")) && !exists(scratch.path("x.part")));
    // Accept, then cancel with the receiver's failure.
    std::vector<protocol::Record> out = recordsFrom(b, "W1AW");
    CHECK_EQ(out.size(), (size_t)2);
    if (out.size() == 2)
    {
        const auto* accept = out[0].as<protocol::FileAnswer>();
        const auto* cancel = out[1].as<protocol::FileCancel>();
        CHECK(accept != nullptr && accept->offer == 3 && accept->accept && accept->from == 0);
        CHECK(cancel != nullptr && cancel->offer == 3 &&
              cancel->reason == (uint8_t)protocol::CancelReason::ReceiverFailed);
    }
}

void testReleasingTheStationCancelsHere()
{
    Scratch scratch("release");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 9000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("x.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turn();

    // With no session up, nothing is said.
    link.a.release("VK3ABC", false);
    link.b.release("W1AW", false);
    CHECK(transferOf(link.a, id).state == State::Cancelled);
    CHECK(last(link.b, false).state == State::Cancelled);
    CHECK(!exists(saveAs + ".part"));
    CHECK(link.a.takeRecords("VK3ABC", link.numA, link.nowMs).empty());
    CHECK(link.b.takeRecords("W1AW", link.numB, link.nowMs).empty());
}

// Letting go of the station while its offer is up, in the session: the
// sender hears it cancelled, not that this station can't take files.
void testReleasingTheStationInASessionCancelsThere()
{
    Scratch scratch("release-offer");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 9000), link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).state == State::Asking);
    link.b.release("W1AW", true);
    CHECK(last(link.b, false).state == State::Cancelled);
    CHECK(link.b.hasRecords("W1AW"));
    link.turn();
    CHECK(transferOf(link.a, id).state == State::CancelledThere);
    CHECK(!link.b.hasRecords("W1AW"));

    // And the other way, mid-file.
    uint64_t second = link.a.offer("VK3ABC", scratch.write("y.bin", 20000), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("y.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(transferOf(link.a, second).state == State::Sending);
    link.a.release("VK3ABC", true);
    link.turn();
    CHECK(transferOf(link.a, second).state == State::Cancelled);
    CHECK(last(link.b, false).state == State::CancelledThere);
    CHECK(exists(saveAs + ".part") && !exists(saveAs) && link.b.keptCount() == 1);
}

// Sending not allowed here: a file going either way ends, the far end
// told; nothing new is offered, sent, accepted or asked a session for.
void testInhibitedSendsNothingMore()
{
    Scratch scratch("inhibit");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("big.bin", 30000), link.nowMs, error);
    uint64_t waiting = link.a.offer("VK3ABC", scratch.write("next.bin", 100), link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("big.got");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    CHECK(transferOf(link.a, id).state == State::Sending);

    link.a.setInhibited(true);
    CHECK(transferOf(link.a, id).state == State::Failed && !transferOf(link.a, id).error.empty());
    CHECK(transferOf(link.a, waiting).state == State::Failed);
    int pieces = link.pieces;
    link.turns(3);
    CHECK(link.pieces == pieces);
    CHECK(last(link.b, false).state == State::FailedThere);
    CHECK(exists(saveAs + ".part") && link.b.keptCount() == 1);

    std::string peer;
    uint64_t since = 0;
    error.clear();
    CHECK(link.a.offer("VK3ABC", scratch.write("more.bin", 10), link.nowMs, error) == 0 && !error.empty());
    CHECK(!link.a.wantsSession(peer, since));
    CHECK(!link.a.busyWith("VK3ABC"));

    // An offer to the inhibited side, even from a station it takes files
    // from without asking, is refused at once.
    link.a.setAutoAccept(scratch.dir.string(), {"VK3ABC"});
    uint64_t theirs = link.b.offer("W1AW", scratch.write("in.bin", 10), link.nowMs, error);
    link.turns(2);
    CHECK(last(link.a, false).state == State::Failed && !last(link.a, false).autoAccepted);
    CHECK(transferOf(link.b, theirs).state == State::FailedThere);

    // Allowed again: files go as before.
    link.a.setInhibited(false);
    uint64_t again = link.a.offer("VK3ABC", scratch.write("again.bin", 100), link.nowMs, error);
    CHECK(again != 0 && link.a.wantsSession(peer, since));
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("again.got"), error));
    link.turns(3);
    CHECK(transferOf(link.a, again).state == State::Delivered);
}

// An offer up when sending stops being allowed is declined in effect, and
// one arriving meanwhile can't be accepted.
void testInhibitedReceiverEndsTheOffer()
{
    Scratch scratch("inhibit-offer");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("x.bin", 10), link.nowMs, error);
    link.turn();
    uint64_t asking = last(link.b, false).id;
    CHECK(last(link.b, false).state == State::Asking);
    link.b.setInhibited(true);
    CHECK(!link.b.accept(asking, scratch.path("x.got"), error));
    CHECK(transferOf(link.b, asking).state == State::Failed);
    link.turn();
    CHECK(transferOf(link.a, id).state == State::FailedThere);
}

void testOfferRefusesWhatCantBeSent()
{
    Scratch scratch("refuse");
    SessionFiles a;
    std::string error;
    CHECK(a.offer("VK3ABC", scratch.path("missing.bin"), 1000, error) == 0 && !error.empty());
    error.clear();
    CHECK(a.offer("VK3ABC", utf8FromPath(scratch.dir), 1000, error) == 0 && !error.empty());
    CHECK(a.transfers().empty());
}

// What the screen shows besides the state: how long an offer has
// left, and how much of a file has been handed to the modem ahead of what
// the far end has acknowledged.
void testTheWindowSeesExpiryAndBytesHanded()
{
    Scratch scratch("shown");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("FRED.TXT", 7000), link.nowMs, error);
    link.turn();
    uint64_t later = link.nowMs + 60 * 1000;
    for (const FileTransfer& t : link.a.transfers(later))
    {
        CHECK(t.state == State::Offered && t.expiresInMs == SessionFiles::OFFER_EXPIRY_MS - 60 * 1000);
    }
    for (const FileTransfer& t : link.b.transfers(later))
    {
        CHECK(t.state == State::Asking && t.expiresInMs == SessionFiles::OFFER_EXPIRY_MS - 60 * 1000);
    }
    CHECK(last(link.b, false).expiresInMs == 0); // no clock given
    for (const FileTransfer& t : link.b.transfers(link.nowMs + SessionFiles::OFFER_EXPIRY_MS + 1))
    {
        CHECK(t.expiresInMs == 0);
    }

    CHECK(link.b.accept(last(link.b, false).id, scratch.path("saved.txt"), error));
    link.turn(); // the Accept reaches A
    // Its first piece is handed over, and not yet acknowledged.
    std::vector<uint8_t> piece;
    CHECK(link.a.takePiece("VK3ABC", link.numA, link.writtenByA, link.nowMs, piece));
    FileTransfer sending = transferOf(link.a, id);
    CHECK(sending.state == State::Sending && sending.done == 0 && sending.handed == 4096);
    CHECK(sending.expiresInMs == 0);
    CHECK(last(link.b, false).handed == last(link.b, false).done);
}

// Progress inside a piece, in steps: the sender's as the far end's modem
// acknowledges the piece's bytes, the receiver's as they arrive, before
// the piece is whole and written.
void testProgressMovesWithinAPiece()
{
    Scratch scratch("within");
    Link link;
    std::string error;
    uint64_t id = link.a.offer("VK3ABC", scratch.write("FRED.TXT", 7000), link.nowMs, error);
    link.turn();
    CHECK(link.b.accept(last(link.b, false).id, scratch.path("saved.txt"), error));
    link.turn(); // the Accept reaches A

    std::vector<uint8_t> piece;
    uint64_t start = link.writtenByA;
    CHECK(link.a.takePiece("VK3ABC", link.numA, start, link.nowMs, piece));
    // Type, a 2-byte length, W1AW's 4-byte call, seq, time and offset.
    const size_t header = 1 + 2 + 4 + 3 + 4 + 4;
    CHECK(piece.size() == header + 4096);
    const uint64_t dataStart = start + header;

    // The header acknowledged: nothing of the file yet.
    link.a.acknowledged("VK3ABC", dataStart);
    CHECK(transferOf(link.a, id).done == 0);
    // 1,000 bytes of it: to the step below.
    link.a.acknowledged("VK3ABC", dataStart + 1000);
    CHECK(transferOf(link.a, id).done == 768);
    link.a.acknowledged("VK3ABC", dataStart + 1100);
    CHECK(transferOf(link.a, id).done == 1024);
    // All of it: the whole piece, not a step short.
    link.a.acknowledged("VK3ABC", start + piece.size());
    CHECK(transferOf(link.a, id).done == 4096);

    // The receiver, the same piece in two reads.
    std::vector<protocol::Record> records;
    CHECK(link.toB.feed(piece.data(), header + 600, records) && records.empty());
    uint32_t number = 0, offset = 99;
    const uint8_t* data = nullptr;
    size_t partial = 0;
    CHECK(link.toB.partialFileData(number, offset, data, partial));
    CHECK(partial == 600 && offset == 0 && data != nullptr);
    link.b.arriving("W1AW", number, offset, data, partial);
    FileTransfer coming = last(link.b, false);
    CHECK(coming.done == 0 && coming.handed == 512);
    // Another number's partial piece is none of this one's.
    std::vector<uint8_t> other(3000, 0x41);
    link.b.arriving("W1AW", number + 1, 0, other.data(), other.size());
    CHECK(last(link.b, false).handed == 512);

    CHECK(link.toB.feed(piece.data() + header + 600, piece.size() - header - 600, records));
    CHECK(records.size() == 1 && !link.toB.partialFileData(number, partial));
    link.b.onRecord("W1AW", records[0], link.nowMs);
    coming = last(link.b, false);
    CHECK(coming.done == 4096 && coming.handed == 4096);
    link.writtenByA += piece.size();
}

// A session lost mid-file: the receiver keeps what came, and when the
// same file comes again from the same station, saved to the same place,
// only the rest is sent.
void testALostTransferCarriesOn()
{
    Scratch scratch("resume");
    Link link;
    std::string source = scratch.write("big.bin", 20000);
    std::string error;
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(3);
    CHECK(last(link.b, false).done == 8192);
    link.a.sessionEnded("VK3ABC", link.nowMs);
    link.b.sessionEnded("W1AW", link.nowMs);
    CHECK(link.b.keptCount() == 1 && exists(saveAs + ".part"));

    // A new session; the decoders start again with it.
    link.newSession();
    uint64_t again = link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    FileTransfer offered = last(link.b, false);
    CHECK(offered.state == State::Asking && offered.kept == 8192 && offered.keptPath == saveAs);
    int pieces = link.pieces;
    CHECK(link.b.accept(offered.id, saveAs, error));
    CHECK(last(link.b, false).done == 8192 && last(link.b, false).resumedFrom == 8192);
    CHECK(link.b.keptCount() == 0);
    link.turns(5);
    CHECK(transferOf(link.a, again).state == State::Delivered && transferOf(link.a, again).resumedFrom == 8192);
    CHECK(link.pieces - pieces == 3); // 11,808 bytes, not 20,000
    CHECK(last(link.b, false).state == State::Saved);
    CHECK(readFile(saveAs) == readFile(source));
    CHECK(!exists(saveAs + ".part"));
}

// Saved somewhere else, it starts again, and what was kept goes.
void testSavedElsewhereStartsAgain()
{
    Scratch scratch("resume-elsewhere");
    Link link;
    std::string source = scratch.write("big.bin", 20000);
    std::string error;
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    std::string first = scratch.path("first.bin");
    CHECK(link.b.accept(last(link.b, false).id, first, error));
    link.turns(2);
    CHECK(link.b.cancel(last(link.b, false).id));
    link.turn();
    CHECK(link.b.keptCount() == 1);

    uint64_t again = link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).kept > 0);
    std::string second = scratch.path("second.bin");
    CHECK(link.b.accept(last(link.b, false).id, second, error));
    CHECK(link.b.keptCount() == 0 && !exists(first + ".part"));
    CHECK(last(link.b, false).resumedFrom == 0);
    link.turns(8);
    CHECK(transferOf(link.a, again).state == State::Delivered);
    CHECK(readFile(second) == readFile(source));
}

// A file that changed since is a different file: nothing kept is offered.
// And one kept that changed on disk is not carried on in.
void testOnlyTheSameFileCarriesOn()
{
    Scratch scratch("resume-changed");
    Link link;
    std::string source = scratch.write("big.bin", 20000);
    std::string error;
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    std::string saveAs = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    link.turns(2);
    link.b.cancel(last(link.b, false).id);
    link.turn();
    CHECK(link.b.keptCount() == 1);

    { std::fstream f(pathFromUtf8(source), std::ios::in | std::ios::out | std::ios::binary); f.seekp(100); f.put('X'); }
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).kept == 0);
    link.b.decline(last(link.b, false).id);
    link.turn();

    // The original again, but the kept part was cut short meanwhile.
    { std::fstream f(pathFromUtf8(source), std::ios::in | std::ios::out | std::ios::binary); f.seekp(100); f.put((char)((100 * 7) & 0xFF)); }
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).kept == 4096);
    fs::resize_file(pathFromUtf8(saveAs + ".part"), 1000);
    CHECK(link.b.accept(last(link.b, false).id, saveAs, error));
    CHECK(last(link.b, false).resumedFrom == 0 && link.b.keptCount() == 0);
    link.turns(8);
    CHECK(last(link.b, false).state == State::Saved && readFile(saveAs) == readFile(source));
}

// From a station on the auto-accept list, it carries on without asking.
void testAutoAcceptCarriesOn()
{
    Scratch scratch("resume-auto");
    Link link;
    fs::create_directories(scratch.dir / "in");
    link.b.setAutoAccept(utf8FromPath(scratch.dir / "in"), {"W1AW"});
    std::string source = scratch.write("FRED.TXT", 20000);
    std::string error;
    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turns(3);
    std::string saveAs = utf8FromPath(scratch.dir / "in" / "FRED.TXT");
    CHECK(last(link.b, false).state == State::Receiving && last(link.b, false).path == saveAs);
    link.a.sessionEnded("VK3ABC", link.nowMs);
    link.b.sessionEnded("W1AW", link.nowMs);
    link.newSession();

    link.a.offer("VK3ABC", source, link.nowMs, error);
    link.turn();
    CHECK(last(link.b, false).autoAccepted && last(link.b, false).resumedFrom > 0);
    link.turns(6);
    CHECK(last(link.b, false).state == State::Saved && last(link.b, false).path == saveAs);
    CHECK(readFile(saveAs) == readFile(source));
}

// What is kept goes after KEPT_MS (7 days).
void testWhatIsKeptAndForHowLong()
{
    Scratch scratch("kept");
    std::string error;
    Link link;
    link.a.offer("VK3ABC", scratch.write("big.bin", 20000), link.nowMs, error);
    link.turn();
    std::string got = scratch.path("got.bin");
    CHECK(link.b.accept(last(link.b, false).id, got, error));
    link.turns(2);
    link.b.sessionEnded("W1AW", link.nowMs);
    CHECK(link.b.keptCount() == 1);
    link.b.tick(link.nowMs + SessionFiles::KEPT_MS - 1);
    CHECK(link.b.keptCount() == 1);
    link.b.tick(link.nowMs + SessionFiles::KEPT_MS);
    CHECK(link.b.keptCount() == 0 && !exists(got + ".part"));
}


// The bytes came, all of them, but they aren't the file offered: nothing
// is saved, and the sender hears it failed here.
void testADigestMismatchIsNotSaved()
{
    Scratch scratch("digest");
    SessionFiles b;
    b.onRecord("W1AW", offerRecord(5, "hello", "h.txt"), NOW);
    std::string error;
    std::string saveAs = scratch.path("h.txt");
    CHECK(b.accept(last(b, false).id, saveAs, error));
    b.onRecord("W1AW", dataRecord(5, "jello"), NOW);
    CHECK(last(b, false).state == State::Failed);
    CHECK(!exists(saveAs) && !exists(saveAs + ".part"));
    std::vector<protocol::Record> out = recordsFrom(b, "W1AW");
    CHECK_EQ(out.size(), (size_t)2);
    if (out.size() == 2)
    {
        const auto* cancel = out[1].as<protocol::FileCancel>();
        CHECK(cancel != nullptr && cancel->offer == 5 &&
              cancel->reason == (uint8_t)protocol::CancelReason::ReceiverFailed);
    }

    // The same bytes, right this time, are saved.
    b.onRecord("W1AW", offerRecord(6, "hello", "h.txt"), NOW);
    CHECK(b.accept(last(b, false).id, saveAs, error));
    b.onRecord("W1AW", dataRecord(6, "hel"), NOW);
    b.onRecord("W1AW", dataRecord(6, "lo", 3), NOW);
    CHECK(last(b, false).state == State::Saved && readFile(saveAs) == "hello");
}

// A FileData that doesn't start where the file has got to ends it.
void testOutOfOrderFails()
{
    Scratch scratch("order");
    SessionFiles b;
    b.onRecord("W1AW", offerRecord(9, "abcdef", "o.txt"), NOW);
    std::string error;
    CHECK(b.accept(last(b, false).id, scratch.path("o.txt"), error));
    b.onRecord("W1AW", dataRecord(9, "abc"), NOW);
    CHECK(last(b, false).state == State::Receiving && last(b, false).done == 3);
    b.onRecord("W1AW", dataRecord(9, "ef", 4), NOW);
    CHECK(last(b, false).state == State::Failed);
    CHECK(last(b, false).error.find("out of order") != std::string::npos);
    CHECK(!exists(scratch.path("o.txt")) && !exists(scratch.path("o.txt.part")));

    // FileData for another offer, or from another station, is dropped.
    b.onRecord("W1AW", offerRecord(10, "abc", "p.txt"), NOW);
    CHECK(b.accept(last(b, false).id, scratch.path("p.txt"), error));
    b.onRecord("W1AW", dataRecord(11, "abc"), NOW);
    b.onRecord("VK3ABC", dataRecord(10, "abc", 0, "VK3ABC"), NOW);
    CHECK(last(b, false).state == State::Receiving && last(b, false).done == 0);
}

// Nothing over 300 KB goes or comes.
void testTheSizeCap()
{
    Scratch scratch("cap");
    SessionFiles a;
    std::string error;
    CHECK(a.offer("VK3ABC", scratch.write("big.bin", SessionFiles::MAX_FILE_BYTES + 1), NOW, error) == 0);
    CHECK(!error.empty());
    error.clear();
    CHECK(a.offer("VK3ABC", scratch.write("just.bin", SessionFiles::MAX_FILE_BYTES), NOW, error) != 0);

    SessionFiles b;
    protocol::Record offer = offerRecord(4, "x", "huge.bin");
    std::get<protocol::FileOffer>(offer.body).size = (uint32_t)SessionFiles::MAX_FILE_BYTES + 1;
    b.onRecord("W1AW", offer, NOW);
    CHECK(last(b, false).state == State::Failed);
    std::vector<protocol::Record> out = recordsFrom(b, "W1AW");
    CHECK_EQ(out.size(), (size_t)1);
    if (out.size() == 1)
    {
        const auto* cancel = out[0].as<protocol::FileCancel>();
        CHECK(cancel != nullptr && cancel->offer == 4 &&
              cancel->reason == (uint8_t)protocol::CancelReason::ReceiverFailed);
    }
}

} // namespace

int main()
{
    testSafeNames();
    testAFileEndToEnd();
    testAnEmptyFile();
    testDeclined();
    testTheSenderCancels();
    testACancelLeavesAPartSent();
    testAPartSentIsForgotten();
    testAPieceStillArrivingIsKept();
    testTheReceiverCancels();
    testAnOfferExpiresOnBothSides();
    testALostSession();
    testAStationThatTakesNoFiles();
    testOneFileAtATime();
    testACancelCrossingSaved();
    testAutoAcceptDoesntReplaceAFileMadeMeanwhile();
    testAutoAcceptNumbersAClash();
    testAHostileNameIsSavedSafely();
    testAPartFileOfTheOperatorsIsLeftAlone();
    testTwoFilesCantBeSavedAsOne();
    testAutoAcceptNeverOverwrites();
    testTheReceiverCantWrite();
    testMoreThanOfferedIsRefused();
    testReleasingTheStationCancelsHere();
    testReleasingTheStationInASessionCancelsThere();
    testInhibitedSendsNothingMore();
    testInhibitedReceiverEndsTheOffer();
    testOfferRefusesWhatCantBeSent();
    testTheWindowSeesExpiryAndBytesHanded();
    testProgressMovesWithinAPiece();
    testALostTransferCarriesOn();
    testSavedElsewhereStartsAgain();
    testOnlyTheSameFileCarriesOn();
    testAutoAcceptCarriesOn();
    testWhatIsKeptAndForHowLong();
    testADigestMismatchIsNotSaved();
    testOutOfOrderFails();
    testTheSizeCap();
    return ragtime_test::finish();
}
