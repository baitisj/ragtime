// Protocol v0: the field codings, every record both ways, broadcast
// frames, digests, sequence numbers and a session's byte stream.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/protocol/Fields.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"

using namespace ragtime::protocol;

namespace
{

constexpr uint32_t TIME = 1792627200; // 2026-10-22 00:00 UTC

std::string hexOf(const std::vector<uint8_t>& bytes)
{
    std::string out;
    char two[3];
    for (uint8_t b : bytes)
    {
        std::snprintf(two, sizeof(two), "%02x", b);
        out += two;
    }
    return out;
}

Record make(Body body, const std::string& call = "AG7EW", uint32_t seq = 7)
{
    Record record;
    record.header.callsign = call;
    record.header.seq = seq;
    record.header.time = TIME;
    record.body = std::move(body);
    return record;
}

// Encodes and decodes one record, checking the header came back.
Record roundTrip(const Record& record)
{
    std::vector<uint8_t> bytes = encodeRecord(record);
    CHECK(!bytes.empty());
    const uint8_t* at = bytes.data();
    Record back;
    CHECK(decodeRecord(at, bytes.data() + bytes.size(), back) == DecodeResult::Ok);
    CHECK(at == bytes.data() + bytes.size());
    CHECK_EQ(back.header.callsign, record.header.callsign);
    CHECK_EQ(back.header.seq, record.header.seq);
    CHECK_EQ(back.header.time, record.header.time);
    CHECK_EQ((int)back.type(), (int)record.type());
    return back;
}

//-------------------------------------------------------------------------
// Fields
//-------------------------------------------------------------------------

void testCallsigns()
{
    CHECK_EQ(normalCallsign("ag7ew"), std::string("AG7EW"));
    CHECK_EQ(normalCallsign("VK2ABC/P"), std::string("VK2ABC/P"));
    CHECK_EQ(normalCallsign("W1AW-7"), std::string("W1AW-7"));
    CHECK_EQ(normalCallsign("AG7EW:board"), std::string()); // ':' is reserved
    CHECK_EQ(normalCallsign("ABCDEFGHIJK"), std::string()); // 11 characters
    CHECK_EQ(normalCallsign(""), std::string());

    // Three characters to two bytes, with an end mark.
    const char* calls[] = {"K", "W1AW", "AG7EW", "ABC", "K1ABC/P", "VE7/AG7EW", "ABCDEFGHIJ", "0-9/"};
    for (const char* call : calls)
    {
        std::vector<uint8_t> out;
        CHECK(putCallsign(out, call));
        CHECK_EQ(out.size(), callsignBytes(call));
        out.push_back(0x55); // what follows is left alone
        Reader in(out.data(), out.data() + out.size());
        std::string back;
        CHECK(in.callsign(back));
        CHECK_EQ(back, std::string(call));
        CHECK_EQ(in.left(), (size_t)1);
    }
    CHECK_EQ(callsignBytes("AG7EW"), (size_t)4);
    CHECK_EQ(callsignBytes("W1AW"), (size_t)4);
    CHECK_EQ(callsignBytes("ABCDEFGHIJ"), (size_t)8);
    std::vector<uint8_t> ag7ew;
    putCallsign(ag7ew, "ag7ew");
    CHECK_EQ(hexOf(ag7ew), std::string("077a22d8"));

    // Not a callsign: refused, the reader left where it was.
    std::vector<uint8_t> bad;
    CHECK(!putCallsign(bad, "N0/CALL?"));
    CHECK(bad.empty());
    const std::vector<std::vector<uint8_t>> broken = {
        {},
        {0x00, 0x00},           // an end mark first: no call
        {0xFF, 0xFF},           // past 40 cubed
        {0x07, 0x0F},           // never ends
        {0x00, 0x29},           // a character after the end mark
    };
    for (const auto& bytes : broken)
    {
        Reader in(bytes.data(), bytes.data() + bytes.size());
        std::string call;
        CHECK(!in.callsign(call));
        CHECK_EQ(in.left(), bytes.size());
    }
}

void testLocators()
{
    for (const char* locator : {"CN87", "CN87ux", "AA00", "RR99", "RR99xx", "FN31pr", "JO62"})
    {
        uint32_t packed = packLocator(locator);
        CHECK(packed != 0);
        std::string back = unpackLocator(packed);
        CHECK_EQ(back, std::string(locator));
    }
    CHECK_EQ(unpackLocator(packLocator("cn87UX")), std::string("CN87ux"));
    for (const char* bad : {"", "CN8", "CN87u", "SN87", "CNA7", "CN87yx", "CN87uxx"}) CHECK_EQ(packLocator(bad), 0u);
    CHECK_EQ(unpackLocator(0), std::string());
    CHECK_EQ(unpackLocator(0xFFFFFFFF), std::string());
}

void testLengths()
{
    for (size_t length : {(size_t)0, (size_t)1, (size_t)127, (size_t)128, (size_t)300, MAX_RECORD_LENGTH})
    {
        std::vector<uint8_t> out;
        putLength(out, length);
        CHECK_EQ(out.size(), length < 128 ? (size_t)1 : (size_t)2);
        size_t back = 0, bytes = 0;
        CHECK(readLength(out.data(), out.data() + out.size(), back, bytes) == LengthRead::Ok);
        CHECK_EQ(back, length);
        CHECK_EQ(bytes, out.size());
        // Cut short.
        CHECK(readLength(out.data(), out.data() + out.size() - 1, back, bytes) == LengthRead::Short);
    }
    std::vector<uint8_t> three = {0x80, 0x80, 0x01};
    size_t back = 0, bytes = 0;
    CHECK(readLength(three.data(), three.data() + 3, back, bytes) == LengthRead::Invalid);
}

//-------------------------------------------------------------------------
// Records
//-------------------------------------------------------------------------

void testEveryRecord()
{
    {
        Hello hello;
        hello.acceptsSessions = true;
        hello.locator = "CN87ux";
        hello.software = "Ragtime 0.1.0";
        Record back = roundTrip(make(hello));
        const Hello* h = back.as<Hello>();
        CHECK(h != nullptr && h->version == 0 && h->acceptsSessions && !h->automatic && h->takes == TAKES_ALL_V0 &&
              h->locator == "CN87ux" && h->software == "Ragtime 0.1.0");
        // The locator and the software are optional.
        Hello plain;
        std::vector<uint8_t> bytes = encodeRecord(make(plain));
        Record shorter = roundTrip(make(plain));
        CHECK(shorter.as<Hello>() != nullptr && shorter.as<Hello>()->locator.empty());
        // A Hello cut after takes still reads.
        Record cut = make(plain);
        cut.body = Unknown{(uint8_t)RecordType::Hello, {0, 1, TAKES_TEXT}};
        Record cutBack = roundTrip(cut);
        CHECK(cutBack.as<Hello>() != nullptr && cutBack.as<Hello>()->acceptsSessions &&
              cutBack.as<Hello>()->takes == TAKES_TEXT);
    }
    {
        Status status;
        status.state = Status::State::Qrl;
        status.partner = "KC2G";
        Record back = roundTrip(make(status));
        CHECK(back.as<Status>() != nullptr && back.as<Status>()->state == Status::State::Qrl &&
              back.as<Status>()->partner == "KC2G");
        Status qrv;
        Record backQrv = roundTrip(make(qrv));
        CHECK(backQrv.as<Status>() != nullptr && backQrv.as<Status>()->partner.empty());
    }
    {
        Text text;
        text.text = "CQ CQ de AG7EW";
        Record back = roundTrip(make(text));
        CHECK(back.as<Text>() != nullptr && back.as<Text>()->text == text.text &&
              back.as<Text>()->coding == CODING_TABLE_0 && back.as<Text>()->to.empty());

        Text addressed;
        addressed.text = "@W1AW are you there?";
        addressed.to = "W1AW";
        addressed.ackWanted = true;
        Record backAddressed = roundTrip(make(addressed));
        const Text* t = backAddressed.as<Text>();
        CHECK(t != nullptr && t->to == "W1AW" && t->ackWanted && t->text == addressed.text);

        // Text that codes longer than it is goes raw.
        Text cyrillic;
        cyrillic.text = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
        Record backRaw = roundTrip(make(cyrillic));
        CHECK(backRaw.as<Text>() != nullptr && backRaw.as<Text>()->coding == CODING_RAW &&
              backRaw.as<Text>()->text == cyrillic.text);
    }
    {
        Ack ack;
        ack.to = "W1AW";
        ack.item = 0x123456;
        Record back = roundTrip(make(ack));
        CHECK(back.as<Ack>() != nullptr && back.as<Ack>()->to == "W1AW" && back.as<Ack>()->item == 0x123456);
    }
    {
        FileOffer offer;
        offer.size = 7000;
        offer.digest = digestOf(std::vector<uint8_t>(7000, 'x'));
        offer.name = "FRED.TXT";
        Record back = roundTrip(make(offer));
        const FileOffer* o = back.as<FileOffer>();
        CHECK(o != nullptr && o->size == 7000 && o->digest == offer.digest && o->name == "FRED.TXT");
        offer.name = std::string(MAX_FILE_NAME + 1, 'n');
        CHECK(encodeRecord(make(offer)).empty());
    }
    {
        FileAnswer accept;
        accept.offer = 3;
        accept.accept = true;
        accept.from = 4096;
        Record back = roundTrip(make(accept));
        const FileAnswer* a = back.as<FileAnswer>();
        CHECK(a != nullptr && a->offer == 3 && a->accept && a->from == 4096);
        FileAnswer decline;
        decline.offer = 3;
        Record backDecline = roundTrip(make(decline));
        CHECK(backDecline.as<FileAnswer>() != nullptr && !backDecline.as<FileAnswer>()->accept);
    }
    {
        FileData data;
        data.offset = 4096;
        data.bytes.assign(4096, 0xAB);
        Record back = roundTrip(make(data, "AG7EW", 3));
        CHECK(back.as<FileData>() != nullptr && back.as<FileData>()->offset == 4096 &&
              back.as<FileData>()->bytes == data.bytes);
    }
    {
        FileSaved saved;
        saved.offer = 3;
        CHECK(roundTrip(make(saved)).as<FileSaved>()->offer == 3);
        FileCancel cancel;
        cancel.offer = 3;
        cancel.reason = (uint8_t)CancelReason::ReceiverStopped;
        Record back = roundTrip(make(cancel));
        CHECK(back.as<FileCancel>()->offer == 3 && back.as<FileCancel>()->reason == 0x81);
    }
    {
        FileAnnounce announce;
        announce.size = 10000;
        announce.pieceBytes = 200;
        announce.digest = digestOf(std::vector<uint8_t>(10000, 1));
        announce.name = "map.png";
        Record back = roundTrip(make(announce));
        const FileAnnounce* a = back.as<FileAnnounce>();
        CHECK(a != nullptr && a->size == 10000 && a->pieceBytes == 200 && a->digest == announce.digest &&
              a->name == "map.png");
        FilePiece piece;
        piece.index = 49;
        piece.bytes.assign(200, 2);
        Record backPiece = roundTrip(make(piece));
        CHECK(backPiece.as<FilePiece>()->index == 49 && backPiece.as<FilePiece>()->bytes.size() == 200);
    }
    {
        Seal seal;
        seal.number = 5;
        seal.previous = digestOf(std::vector<uint8_t>(1, 1));
        seal.firstItem = 41;
        for (int i = 0; i < 4; i++) seal.items.push_back(digestOf(std::vector<uint8_t>(1, (uint8_t)i)));
        Record back = roundTrip(make(seal));
        const Seal* s = back.as<Seal>();
        CHECK(s != nullptr && s->number == 5 && s->previous == seal.previous && s->firstItem == 41 &&
              s->items == seal.items);
    }
    {
        RepairRequest request;
        request.to = "AG7EW";
        request.seal = 5;
        request.sealDigest = digestOf(std::vector<uint8_t>(1, 9));
        request.missingItems = {false, true, false, false, true, false, false, false, false, true};
        request.files.push_back({42, {3, 4, 5, 9}});
        request.files.push_back({44, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26}});
        Record back = roundTrip(make(request, "W1AW", 12));
        const RepairRequest* r = back.as<RepairRequest>();
        CHECK(r != nullptr && r->to == "AG7EW" && r->seal == 5 && r->sealDigest == request.sealDigest &&
              !r->wantSeal && !r->snr && r->missingItems == request.missingItems && r->files == request.files);

        // The asker's S/N rides in the flags byte, beside "send the seal
        // again", and the request is no longer for it.
        std::vector<uint8_t> plain = encodeRecord(make(request, "W1AW", 12));
        request.snr = -3.2;
        request.wantSeal = true;
        std::vector<uint8_t> withSnr = encodeRecord(make(request, "W1AW", 12));
        CHECK_EQ(withSnr.size(), plain.size());
        back = roundTrip(make(request, "W1AW", 12));
        r = back.as<RepairRequest>();
        CHECK(r != nullptr && r->wantSeal && r->snr && *r->snr == -3.0 && r->files == request.files);
        // 0.1 reads only bit 0: the byte after the digest.
        size_t flags = 2 + 4 + 3 + 4 + 4 + 2 + 12;   // type, length, W1AW, seq, time, to AG7EW, seal, digest
        CHECK_EQ(withSnr[flags] & 0x01, 1);
        CHECK_EQ(withSnr[flags] >> 1, (int)repairSnrCode(-3.0));
    }
    {
        // The S/N code: none, half-dB steps, held at both ends.
        CHECK_EQ((int)repairSnrCode(std::nullopt), 0);
        CHECK(!repairSnr(0));
        CHECK_EQ((int)repairSnrCode(-24.0), 1);
        CHECK_EQ((int)repairSnrCode(-40.0), 1);
        CHECK_EQ((int)repairSnrCode(39.0), 127);
        CHECK_EQ((int)repairSnrCode(80.0), 127);
        CHECK(*repairSnr(repairSnrCode(12.5)) == 12.5);
        CHECK(*repairSnr(repairSnrCode(12.6)) == 12.5);
        CHECK(*repairSnr(repairSnrCode(12.8)) == 13.0);
        for (int code = 1; code <= 127; code++) CHECK_EQ((int)repairSnrCode(repairSnr((uint8_t)code)), code);
    }
    {
        RepairRefused refused;
        refused.seal = 12;
        refused.reason = (uint8_t)RefusalReason::Cancelled;
        refused.files = {44, 0xFFFFFF};
        Record record = make(refused, "AG7EW", 46);
        CHECK_EQ(encodeRecord(record).size(), (size_t)(13 + 2 + 1 + 1 + 6));
        Record back = roundTrip(record);
        const RepairRefused* r = back.as<RepairRefused>();
        CHECK(r != nullptr && r->seal == 12 && r->reason == 1 && r->files == refused.files);
        CHECK_EQ((int)back.type(), 0x42);
        // Whole seal: no files. Bytes past the fields are ignored.
        refused.files.clear();
        refused.reason = (uint8_t)RefusalReason::Closed;
        std::vector<uint8_t> bytes = encodeRecord(make(refused, "AG7EW", 46));
        CHECK_EQ(bytes.size(), (size_t)17);
        bytes.push_back(0x55);
        bytes[1]++;
        const uint8_t* at = bytes.data();
        Record grown;
        CHECK(decodeRecord(at, bytes.data() + bytes.size(), grown) == DecodeResult::Ok);
        r = grown.as<RepairRefused>();
        CHECK(r != nullptr && r->seal == 12 && r->reason == 3 && r->files.empty());
    }
}

void testPieceLists()
{
    const std::vector<std::vector<uint16_t>> lists = {
        {},
        {7},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},                         // one range
        {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26},    // a bitmap is shorter
        {1000, 1001, 2000, 2001, 2002},
        {65535},
    };
    for (const auto& list : lists)
    {
        std::vector<uint8_t> out;
        putPieceList(out, list);
        Reader in(out.data(), out.data() + out.size());
        std::vector<uint16_t> back;
        CHECK(readPieceList(in, back));
        CHECK(in.done());
        CHECK(back == list);
    }
    // A long run splits into ranges of 256.
    std::vector<uint16_t> run;
    for (uint16_t p = 0; p < 600; p++) run.push_back(p);
    std::vector<uint8_t> out;
    putPieceList(out, run);
    Reader in(out.data(), out.data() + out.size());
    std::vector<uint16_t> back;
    CHECK(readPieceList(in, back) && back == run);
    // Out of order and repeated: sorted, once each.
    out.clear();
    putPieceList(out, {9, 3, 3, 5});
    Reader again(out.data(), out.data() + out.size());
    back.clear();
    CHECK(readPieceList(again, back) && back == std::vector<uint16_t>({3, 5, 9}));
}

void testGoldenText()
{
    // docs/PROTOCOL.md's worked example: AG7EW's item 7, "CQ CQ de AG7EW",
    // at 2026-10-22 00:00 UTC, in a broadcast frame.
    Text text;
    text.text = "CQ CQ de AG7EW";
    std::vector<uint8_t> frame = encodeFrame({make(text)});
    // Worked out by hand (and in Python) from the spec, not from this code.
    CHECK_EQ(hexOf(frame), std::string("a0" "10" "14" "077a22d8" "000007" "6ad95200" "00" "00c21d32450427c0"));
    CHECK_EQ(frame.size(), (size_t)23);
    CHECK_EQ(hex(recordDigest(make(text))), std::string("8c8208b632620e86b8884f31"));
}

void testDigests()
{
    Text text;
    text.text = "nice signal, 73";
    Record record = make(text);
    Digest digest = recordDigest(record);

    // The same record decoded gives the same digest, whatever coding it took.
    Record back = roundTrip(record);
    CHECK(recordDigest(back) == digest);
    Record raw = record;
    raw.as<Text>()->coding = CODING_RAW;
    CHECK(recordDigest(raw) == digest);

    // Every signed field counts: the text, the call, the number, the time.
    Record other = record;
    other.as<Text>()->text = "nice signal, 72";
    CHECK(recordDigest(other) != digest);
    other = record;
    other.header.callsign = "AG7EX";
    CHECK(recordDigest(other) != digest);
    other = record;
    other.header.seq++;
    CHECK(recordDigest(other) != digest);
    other = record;
    other.header.time++;
    CHECK(recordDigest(other) != digest);
    other = record;
    other.as<Text>()->to = "W1AW";
    CHECK(recordDigest(other) != digest);
}

void testFrames()
{
    Text a, b;
    a.text = "first";
    b.text = "second";
    Hello hello;
    std::vector<uint8_t> frame = encodeFrame({make(hello), make(a, "AG7EW", 1), make(b, "AG7EW", 2)});
    CHECK(frame.size() > 1 && frame[0] == FRAME_BYTE);
    std::vector<Record> records;
    CHECK(decodeFrame(frame, records));
    CHECK_EQ(records.size(), (size_t)3);
    if (records.size() == 3)
    {
        CHECK(records[1].as<Text>() != nullptr && records[1].as<Text>()->text == "first");
        CHECK(records[2].as<Text>() != nullptr && records[2].as<Text>()->text == "second");
    }

    // Not ours: no frame byte, or a frame byte of a later version.
    records.clear();
    std::vector<uint8_t> notOurs = frame;
    notOurs[0] = 0xA1;
    CHECK(!decodeFrame(notOurs, records));
    CHECK(records.empty());
    CHECK(!decodeFrame(std::vector<uint8_t>(), records));

    // A type this version doesn't know is kept, and the rest still read.
    std::vector<uint8_t> withUnknown(1, FRAME_BYTE);
    Record unknown = make(Unknown{0x7E, {1, 2, 3}});
    encodeRecord(unknown, withUnknown);
    encodeRecord(make(a), withUnknown);
    records.clear();
    CHECK(decodeFrame(withUnknown, records));
    CHECK_EQ(records.size(), (size_t)2);
    if (records.size() == 2)
    {
        CHECK(records[0].as<Unknown>() != nullptr && records[0].type() == 0x7E &&
              records[0].as<Unknown>()->body == std::vector<uint8_t>({1, 2, 3}));
        CHECK(records[1].as<Text>() != nullptr);
    }

    // A malformed record is skipped by its length; a broken one ends the frame.
    std::vector<uint8_t> malformed(1, FRAME_BYTE);
    malformed.insert(malformed.end(), {(uint8_t)RecordType::Ack, 2, 0xFF, 0xFF});
    encodeRecord(make(a), malformed);
    records.clear();
    CHECK(decodeFrame(malformed, records));
    CHECK_EQ(records.size(), (size_t)1);
    std::vector<uint8_t> broken = frame;
    broken.resize(broken.size() - 3);
    records.clear();
    CHECK(!decodeFrame(broken, records));
    CHECK_EQ(records.size(), (size_t)2);
    std::vector<uint8_t> typeZero = {FRAME_BYTE, 0x00, 0x00};
    CHECK(!decodeFrame(typeZero, records));

    // Records that can't be coded aren't.
    Text badCall;
    badCall.text = "x";
    CHECK(encodeFrame({make(badCall, "NOT A CALL")}).empty());
    Record bigSeq = make(a);
    bigSeq.header.seq = 0x1000000;
    CHECK(encodeRecord(bigSeq).empty());
    FileData tooBig;
    tooBig.bytes.assign(MAX_RECORD_LENGTH, 0);
    CHECK(encodeRecord(make(tooBig)).empty());
}

void testABroadcastMessageFits()
{
    // A long chat line still fits one broadcast frame.
    Text text;
    text.text = "Thanks for the nice report, your signal is very good here in Portland. Name here is Jeff, rig "
                "IC-7100 at 50 w into a dipole up 30 ft. Weather is wet tonight. Hope to copy you agn soon, 73";
    std::vector<uint8_t> frame = encodeFrame({make(text, "VE7/AG7EW", 0xFFFFFF)});
    std::printf("a %zu-character line: a %zu-byte frame\n", text.text.size(), frame.size());
    CHECK(frame.size() <= MAX_BROADCAST_FRAME);
    CHECK(frame.size() < text.text.size());
}

//-------------------------------------------------------------------------
// Sequence numbers and the session stream
//-------------------------------------------------------------------------

void testNumbering()
{
    Numbering n("AG7EW");
    CHECK_EQ(n.newestItem(), 0u);
    Hello hello;
    CHECK_EQ(n.record(hello, TIME).header.seq, 0u);        // before any item
    Text text;
    CHECK_EQ(n.record(text, TIME).header.seq, 1u);
    FileOffer offer;
    CHECK_EQ(n.record(offer, TIME).header.seq, 2u);
    FileData data;
    CHECK_EQ(n.record(data, TIME, 2).header.seq, 2u);     // its item's
    FileAnswer answer;
    CHECK_EQ(n.record(answer, TIME).header.seq, 2u);      // the newest item's
    CHECK_EQ(n.record(text, TIME).header.seq, 3u);
    CHECK_EQ(n.record(text, TIME + 5).header.callsign, std::string("AG7EW"));

    // Kept across a restart, and round again after 0xFFFFFF, never 0.
    Numbering resumed("AG7EW", 0xFFFFFE);
    CHECK_EQ(resumed.record(text, TIME).header.seq, 0xFFFFFFu);
    CHECK_EQ(resumed.record(text, TIME).header.seq, 1u);
}

std::vector<uint8_t> sessionBytes(Numbering& n, std::vector<uint32_t>& offers)
{
    Hello hello;
    hello.acceptsSessions = true;
    std::vector<uint8_t> stream = streamOpening(n.record(hello, TIME));
    Text text;
    text.text = "hello there";
    encodeRecord(n.record(text, TIME), stream);
    FileOffer offer;
    offer.size = 5000;
    offer.name = "x.bin";
    Record offerRecord = n.record(offer, TIME);
    offers.push_back(offerRecord.header.seq);
    encodeRecord(offerRecord, stream);
    FileData data;
    data.offset = 0;
    data.bytes.assign(4096, 7);
    encodeRecord(n.record(data, TIME, offerRecord.header.seq), stream);
    return stream;
}

void testTheSessionStream()
{
    Numbering n("W1AW");
    std::vector<uint32_t> offers;
    std::vector<uint8_t> stream = sessionBytes(n, offers);

    // Byte by byte, it reads as it went.
    StreamReader reader;
    std::vector<Record> records;
    bool sawPartial = false;
    for (size_t i = 0; i < stream.size(); i++)
    {
        CHECK(reader.feed(&stream[i], 1, records));
        uint32_t offer = 0;
        size_t bytes = 0;
        if (reader.partialFileData(offer, bytes))
        {
            CHECK_EQ(offer, offers[0]);
            CHECK(bytes < 4096);
            sawPartial = sawPartial || bytes > 2000;
        }
    }
    CHECK(sawPartial);
    CHECK(reader.greeted());
    CHECK_EQ(reader.peer(), std::string("W1AW"));
    CHECK(reader.hello().acceptsSessions);
    CHECK_EQ(records.size(), (size_t)4);
    if (records.size() == 4)
    {
        CHECK(records[0].as<Hello>() != nullptr);
        CHECK(records[1].as<Text>() != nullptr && records[1].as<Text>()->text == "hello there");
        CHECK(records[3].as<FileData>() != nullptr && records[3].header.seq == offers[0]);
    }
    uint32_t offer = 0;
    size_t bytes = 0;
    CHECK(!reader.partialFileData(offer, bytes));

    // All at once.
    StreamReader whole;
    records.clear();
    CHECK(whole.feed(stream, records));
    CHECK_EQ(records.size(), (size_t)4);

    // Not ours: another program's bytes, or records before a Hello.
    StreamReader foreign;
    records.clear();
    std::string other = "G\x05hello";
    CHECK(!foreign.feed((const uint8_t*)other.data(), other.size(), records));
    CHECK(foreign.foreign());
    CHECK(!foreign.feed(stream, records));
    CHECK(records.empty());

    StreamReader noHello;
    Text text;
    std::vector<uint8_t> headless(1, FRAME_BYTE);
    encodeRecord(n.record(text, TIME), headless);
    CHECK(!noHello.feed(headless, records));

    // Broken framing after the Hello: nothing more is taken.
    StreamReader broken;
    std::vector<uint8_t> bad = stream;
    bad.insert(bad.end(), {0x00, 0x01, 0x02});
    records.clear();
    CHECK(!broken.feed(bad, records));
    CHECK(broken.foreign());

    // A malformed record is skipped, and reading goes on.
    StreamReader skips;
    std::vector<uint8_t> withBad = streamOpening(n.record(Hello(), TIME));
    withBad.insert(withBad.end(), {(uint8_t)RecordType::FileSaved, 1, 0x00});
    encodeRecord(n.record(text, TIME), withBad);
    records.clear();
    CHECK(skips.feed(withBad, records));
    CHECK_EQ(records.size(), (size_t)2);
    CHECK_EQ(skips.malformed(), (size_t)1);

    skips.reset();
    CHECK(!skips.greeted());
}

void testNeverAllZero()
{
    // The frame byte opens every frame and stream, and no record type is 0.
    CHECK(FRAME_BYTE != 0);
    Text empty;
    std::vector<uint8_t> frame = encodeFrame({make(empty, "K", 0)});
    bool anyNonZero = false;
    for (uint8_t b : frame) anyNonZero = anyNonZero || b != 0;
    CHECK(anyNonZero);
    CHECK(streamOpening(make(empty)).empty()); // a stream opens only with a Hello
}

} // namespace

int main()
{
    testCallsigns();
    testLocators();
    testLengths();
    testEveryRecord();
    testPieceLists();
    testGoldenText();
    testDigests();
    testFrames();
    testABroadcastMessageFits();
    testNumbering();
    testTheSessionStream();
    testNeverAllZero();
    return ragtime_test::finish();
}
