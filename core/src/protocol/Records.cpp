#include "ragtime/protocol/Records.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>

#include "ragtime/files/Sha256.h"
#include "ragtime/text/HamText.h"

namespace ragtime
{
namespace protocol
{

namespace
{

constexpr uint8_t HELLO_ACCEPTS_SESSIONS = 0x01;
constexpr uint8_t HELLO_AUTOMATIC = 0x02;

constexpr uint8_t TEXT_CODING_MASK = 0x0F;
constexpr uint8_t TEXT_ADDRESSED = 0x10;
constexpr uint8_t TEXT_ACK_WANTED = 0x20;

constexpr uint8_t REPAIR_WANT_SEAL = 0x01;
constexpr int REPAIR_SNR_SHIFT = 1;
constexpr uint8_t REPAIR_SNR_CODES = 127;

constexpr uint32_t MAX_SEQ = 0xFFFFFF;

bool putShortString(std::vector<uint8_t>& out, const std::string& text, size_t limit)
{
    if (text.size() > limit || text.size() > 255) return false;
    out.push_back((uint8_t)text.size());
    out.insert(out.end(), text.begin(), text.end());
    return true;
}

void putDigest(std::vector<uint8_t>& out, const Digest& digest)
{
    out.insert(out.end(), digest.begin(), digest.end());
}

// The body's bytes. canonical: a Text's UTF-8 as it is, whatever coding
// would be chosen on the air (recordDigest).
bool encodeBody(const Body& body, std::vector<uint8_t>& out, bool canonical)
{
    return std::visit(
        [&](const auto& b) -> bool {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Unknown>)
            {
                out.insert(out.end(), b.body.begin(), b.body.end());
                return true;
            }
            else if constexpr (std::is_same_v<T, Hello>)
            {
                out.push_back(b.version);
                out.push_back((uint8_t)((b.acceptsSessions ? HELLO_ACCEPTS_SESSIONS : 0) |
                                        (b.automatic ? HELLO_AUTOMATIC : 0)));
                out.push_back(b.takes);
                putNumber(out, packLocator(b.locator), 4);
                if (!b.software.empty() && !putShortString(out, b.software, 64)) return false;
                return true;
            }
            else if constexpr (std::is_same_v<T, Status>)
            {
                out.push_back((uint8_t)b.state);
                if (!b.partner.empty() && !putCallsign(out, b.partner)) return false;
                return true;
            }
            else if constexpr (std::is_same_v<T, Text>)
            {
                std::vector<uint8_t> coded;
                uint8_t coding = CODING_RAW;
                if (!canonical)
                {
                    coded = text::HamText::encode(b.text);
                    if (coded.size() <= b.text.size()) coding = CODING_TABLE_0;
                }
                bool addressed = !b.to.empty();
                out.push_back((uint8_t)(coding | (addressed ? TEXT_ADDRESSED : 0) |
                                        (addressed && b.ackWanted ? TEXT_ACK_WANTED : 0)));
                if (addressed && !putCallsign(out, b.to)) return false;
                if (coding == CODING_RAW) out.insert(out.end(), b.text.begin(), b.text.end());
                else out.insert(out.end(), coded.begin(), coded.end());
                return true;
            }
            else if constexpr (std::is_same_v<T, Ack>)
            {
                if (!putCallsign(out, b.to) || b.item > MAX_SEQ) return false;
                putNumber(out, b.item, 3);
                return true;
            }
            else if constexpr (std::is_same_v<T, Ping>)
            {
                if (!putCallsign(out, b.to)) return false;
                putNumber(out, b.number, 2);
                return putShortString(out, b.mode, 255);
            }
            else if constexpr (std::is_same_v<T, Pong>)
            {
                if (!putCallsign(out, b.to)) return false;
                putNumber(out, b.number, 2);
                out.push_back(repairSnrCode(b.snr));
                return true;
            }
            else if constexpr (std::is_same_v<T, FileOffer>)
            {
                putNumber(out, b.size, 4);
                putDigest(out, b.digest);
                return putShortString(out, b.name, MAX_FILE_NAME);
            }
            else if constexpr (std::is_same_v<T, FileAnswer>)
            {
                if (b.offer > MAX_SEQ) return false;
                putNumber(out, b.offer, 3);
                out.push_back(b.accept ? 0 : 1);
                if (b.accept) putNumber(out, b.from, 4);
                return true;
            }
            else if constexpr (std::is_same_v<T, FileData>)
            {
                putNumber(out, b.offset, 4);
                out.insert(out.end(), b.bytes.begin(), b.bytes.end());
                return true;
            }
            else if constexpr (std::is_same_v<T, FileSaved>)
            {
                if (b.offer > MAX_SEQ) return false;
                putNumber(out, b.offer, 3);
                return true;
            }
            else if constexpr (std::is_same_v<T, FileCancel>)
            {
                if (b.offer > MAX_SEQ) return false;
                putNumber(out, b.offer, 3);
                out.push_back(b.reason);
                return true;
            }
            else if constexpr (std::is_same_v<T, FileAnnounce>)
            {
                putNumber(out, b.size, 4);
                putNumber(out, b.pieceBytes, 2);
                putDigest(out, b.digest);
                return putShortString(out, b.name, MAX_FILE_NAME);
            }
            else if constexpr (std::is_same_v<T, FilePiece>)
            {
                putNumber(out, b.index, 2);
                out.insert(out.end(), b.bytes.begin(), b.bytes.end());
                return true;
            }
            else if constexpr (std::is_same_v<T, Seal>)
            {
                if (b.firstItem > MAX_SEQ || b.items.size() > 255) return false;
                putNumber(out, b.number, 2);
                putDigest(out, b.previous);
                putNumber(out, b.firstItem, 3);
                out.push_back((uint8_t)b.items.size());
                for (const Digest& d : b.items) putDigest(out, d);
                return true;
            }
            else if constexpr (std::is_same_v<T, RepairRequest>)
            {
                if (!putCallsign(out, b.to) || b.missingItems.size() > 255) return false;
                putNumber(out, b.seal, 2);
                putDigest(out, b.sealDigest);
                out.push_back((uint8_t)((b.wantSeal ? REPAIR_WANT_SEAL : 0) | (repairSnrCode(b.snr) << REPAIR_SNR_SHIFT)));
                out.push_back((uint8_t)b.missingItems.size());
                std::vector<uint8_t> bitmap((b.missingItems.size() + 7) / 8, 0);
                for (size_t i = 0; i < b.missingItems.size(); i++)
                {
                    if (b.missingItems[i]) bitmap[i / 8] |= (uint8_t)(0x80 >> (i % 8));
                }
                out.insert(out.end(), bitmap.begin(), bitmap.end());
                out.push_back(b.itemsWant);
                for (const auto& file : b.files)
                {
                    if (file.first > MAX_SEQ) return false;
                    putNumber(out, file.first, 3);
                    putNumber(out, file.second, 2);
                }
                return true;
            }
            else if constexpr (std::is_same_v<T, RepairRefused>)
            {
                if (b.files.size() > 255) return false;
                putNumber(out, b.seal, 2);
                out.push_back(b.reason);
                out.push_back((uint8_t)b.files.size());
                for (uint32_t seq : b.files)
                {
                    if (seq > MAX_SEQ) return false;
                    putNumber(out, seq, 3);
                }
                return true;
            }
            else if constexpr (std::is_same_v<T, ItemParity>)
            {
                if (b.items.size() > 255) return false;
                putNumber(out, b.seal, 2);
                out.push_back((uint8_t)b.items.size());
                std::vector<uint8_t> bitmap((b.items.size() + 7) / 8, 0);
                for (size_t i = 0; i < b.items.size(); i++)
                {
                    if (b.items[i]) bitmap[i / 8] |= (uint8_t)(0x80 >> (i % 8));
                }
                out.insert(out.end(), bitmap.begin(), bitmap.end());
                putNumber(out, b.index, 2);
                out.insert(out.end(), b.bytes.begin(), b.bytes.end());
                return true;
            }
        },
        body);
}

bool putHeader(const Record& record, std::vector<uint8_t>& out)
{
    if (record.header.seq > MAX_SEQ || !putCallsign(out, record.header.callsign)) return false;
    putNumber(out, record.header.seq, 3);
    putNumber(out, record.header.time, 4);
    return true;
}

template <typename T>
DecodeResult decoded(Record& out, T&& body)
{
    out.body = std::forward<T>(body);
    return DecodeResult::Ok;
}

DecodeResult decodeBody(uint8_t type, Reader& in, Record& out)
{
    switch ((RecordType)type)
    {
        case RecordType::Hello:
        {
            Hello b;
            uint8_t flags = 0;
            if (!in.u8(b.version) || !in.u8(flags) || !in.u8(b.takes)) return DecodeResult::Malformed;
            b.acceptsSessions = (flags & HELLO_ACCEPTS_SESSIONS) != 0;
            b.automatic = (flags & HELLO_AUTOMATIC) != 0;
            uint32_t locator = 0;
            if (!in.done() && !in.u32(locator)) return DecodeResult::Malformed;
            b.locator = unpackLocator(locator);
            if (!in.done() && !in.shortString(b.software)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::Status:
        {
            Status b;
            uint8_t state = 0;
            if (!in.u8(state)) return DecodeResult::Malformed;
            b.state = (Status::State)state;
            if (!in.done() && !in.callsign(b.partner)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::Text:
        {
            Text b;
            uint8_t flags = 0;
            if (!in.u8(flags)) return DecodeResult::Malformed;
            if ((flags & TEXT_ADDRESSED) != 0 && !in.callsign(b.to)) return DecodeResult::Malformed;
            b.ackWanted = (flags & TEXT_ACK_WANTED) != 0 && !b.to.empty();
            b.coding = flags & TEXT_CODING_MASK;
            std::vector<uint8_t> rest = in.rest();
            if (b.coding == CODING_RAW) b.text.assign(rest.begin(), rest.end());
            else if (b.coding == CODING_TABLE_0) b.text = text::HamText::decode(rest);
            else return DecodeResult::Malformed; // a table this version doesn't have
            return decoded(out, std::move(b));
        }
        case RecordType::Ack:
        {
            Ack b;
            if (!in.callsign(b.to) || !in.u24(b.item)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::Ping:
        {
            Ping b;
            if (!in.callsign(b.to) || !in.u16(b.number) || !in.shortString(b.mode)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::Pong:
        {
            Pong b;
            uint8_t code = 0;
            if (!in.callsign(b.to) || !in.u16(b.number) || !in.u8(code)) return DecodeResult::Malformed;
            b.snr = repairSnr(code);
            return decoded(out, std::move(b));
        }
        case RecordType::FileOffer:
        {
            FileOffer b;
            if (!in.u32(b.size) || !in.digest(b.digest) || !in.shortString(b.name)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::FileAnswer:
        {
            FileAnswer b;
            uint8_t answer = 0;
            if (!in.u24(b.offer) || !in.u8(answer)) return DecodeResult::Malformed;
            b.accept = answer == 0;
            if (b.accept && !in.done() && !in.u32(b.from)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::FileData:
        {
            FileData b;
            if (!in.u32(b.offset)) return DecodeResult::Malformed;
            b.bytes = in.rest();
            return decoded(out, std::move(b));
        }
        case RecordType::FileSaved:
        {
            FileSaved b;
            if (!in.u24(b.offer)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::FileCancel:
        {
            FileCancel b;
            if (!in.u24(b.offer) || !in.u8(b.reason)) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::FileAnnounce:
        {
            FileAnnounce b;
            if (!in.u32(b.size) || !in.u16(b.pieceBytes) || !in.digest(b.digest) || !in.shortString(b.name))
            {
                return DecodeResult::Malformed;
            }
            if (b.pieceBytes == 0) return DecodeResult::Malformed;
            return decoded(out, std::move(b));
        }
        case RecordType::FilePiece:
        {
            FilePiece b;
            if (!in.u16(b.index)) return DecodeResult::Malformed;
            b.bytes = in.rest();
            return decoded(out, std::move(b));
        }
        case RecordType::Seal:
        {
            Seal b;
            uint8_t count = 0;
            if (!in.u16(b.number) || !in.digest(b.previous) || !in.u24(b.firstItem) || !in.u8(count))
            {
                return DecodeResult::Malformed;
            }
            b.items.resize(count);
            for (Digest& d : b.items)
            {
                if (!in.digest(d)) return DecodeResult::Malformed;
            }
            return decoded(out, std::move(b));
        }
        case RecordType::RepairRequest:
        {
            RepairRequest b;
            uint8_t flags = 0, count = 0;
            if (!in.callsign(b.to) || !in.u16(b.seal) || !in.digest(b.sealDigest) || !in.u8(flags) ||
                !in.u8(count))
            {
                return DecodeResult::Malformed;
            }
            b.wantSeal = (flags & REPAIR_WANT_SEAL) != 0;
            b.snr = repairSnr((uint8_t)(flags >> REPAIR_SNR_SHIFT));
            std::vector<uint8_t> bitmap;
            if (!in.bytes(bitmap, ((size_t)count + 7) / 8)) return DecodeResult::Malformed;
            b.missingItems.resize(count);
            for (size_t i = 0; i < count; i++) b.missingItems[i] = (bitmap[i / 8] & (0x80 >> (i % 8))) != 0;
            if (!in.u8(b.itemsWant)) return DecodeResult::Malformed;
            while (!in.done())
            {
                std::pair<uint32_t, uint16_t> file;
                if (!in.u24(file.first) || !in.u16(file.second)) return DecodeResult::Malformed;
                b.files.push_back(file);
            }
            return decoded(out, std::move(b));
        }
        case RecordType::RepairRefused:
        {
            RepairRefused b;
            uint8_t count = 0;
            if (!in.u16(b.seal) || !in.u8(b.reason) || !in.u8(count)) return DecodeResult::Malformed;
            b.files.resize(count);
            for (uint32_t& seq : b.files)
            {
                if (!in.u24(seq)) return DecodeResult::Malformed;
            }
            return decoded(out, std::move(b));
        }
        case RecordType::ItemParity:
        {
            ItemParity b;
            uint8_t count = 0;
            if (!in.u16(b.seal) || !in.u8(count)) return DecodeResult::Malformed;
            std::vector<uint8_t> bitmap;
            if (!in.bytes(bitmap, ((size_t)count + 7) / 8) || !in.u16(b.index)) return DecodeResult::Malformed;
            b.items.resize(count);
            for (size_t i = 0; i < count; i++) b.items[i] = (bitmap[i / 8] & (0x80 >> (i % 8))) != 0;
            b.bytes = in.rest();
            return decoded(out, std::move(b));
        }
    }
    Unknown b;
    b.type = type;
    b.body = in.rest();
    return decoded(out, std::move(b));
}

} // namespace

uint8_t repairSnrCode(std::optional<double> dB)
{
    if (!dB) return 0;
    double steps = std::round((*dB - REPAIR_SNR_LOWEST_DB) / REPAIR_SNR_STEP_DB);
    steps = std::min(std::max(steps, 0.0), (double)(REPAIR_SNR_CODES - 1));
    return (uint8_t)(1 + (int)steps);
}

std::optional<double> repairSnr(uint8_t code)
{
    if (code == 0 || code > REPAIR_SNR_CODES) return std::nullopt;
    return REPAIR_SNR_LOWEST_DB + (code - 1) * REPAIR_SNR_STEP_DB;
}

bool isItem(RecordType type)
{
    return type == RecordType::Text || type == RecordType::FileOffer || type == RecordType::FileAnnounce;
}

bool isPartOfItem(RecordType type)
{
    return type == RecordType::FileData || type == RecordType::FilePiece;
}

uint8_t Record::type() const
{
    return std::visit(
        [](const auto& b) -> uint8_t {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Unknown>) return b.type;
            else if constexpr (std::is_same_v<T, Hello>) return (uint8_t)RecordType::Hello;
            else if constexpr (std::is_same_v<T, Status>) return (uint8_t)RecordType::Status;
            else if constexpr (std::is_same_v<T, Text>) return (uint8_t)RecordType::Text;
            else if constexpr (std::is_same_v<T, Ack>) return (uint8_t)RecordType::Ack;
            else if constexpr (std::is_same_v<T, Ping>) return (uint8_t)RecordType::Ping;
            else if constexpr (std::is_same_v<T, Pong>) return (uint8_t)RecordType::Pong;
            else if constexpr (std::is_same_v<T, FileOffer>) return (uint8_t)RecordType::FileOffer;
            else if constexpr (std::is_same_v<T, FileAnswer>) return (uint8_t)RecordType::FileAnswer;
            else if constexpr (std::is_same_v<T, FileData>) return (uint8_t)RecordType::FileData;
            else if constexpr (std::is_same_v<T, FileSaved>) return (uint8_t)RecordType::FileSaved;
            else if constexpr (std::is_same_v<T, FileCancel>) return (uint8_t)RecordType::FileCancel;
            else if constexpr (std::is_same_v<T, FileAnnounce>) return (uint8_t)RecordType::FileAnnounce;
            else if constexpr (std::is_same_v<T, FilePiece>) return (uint8_t)RecordType::FilePiece;
            else if constexpr (std::is_same_v<T, Seal>) return (uint8_t)RecordType::Seal;
            else if constexpr (std::is_same_v<T, RepairRequest>) return (uint8_t)RecordType::RepairRequest;
            else if constexpr (std::is_same_v<T, RepairRefused>) return (uint8_t)RecordType::RepairRefused;
            else return (uint8_t)RecordType::ItemParity;
        },
        body);
}

bool encodeRecord(const Record& record, std::vector<uint8_t>& out)
{
    uint8_t type = record.type();
    if (type == 0) return false;
    std::vector<uint8_t> inner;
    if (!putHeader(record, inner) || !encodeBody(record.body, inner, false)) return false;
    if (inner.size() > MAX_RECORD_LENGTH) return false;
    out.push_back(type);
    putLength(out, inner.size());
    out.insert(out.end(), inner.begin(), inner.end());
    return true;
}

std::vector<uint8_t> encodeRecord(const Record& record)
{
    std::vector<uint8_t> out;
    encodeRecord(record, out);
    return out;
}

std::vector<uint8_t> encodeFrame(const std::vector<Record>& records)
{
    std::vector<uint8_t> out(1, FRAME_BYTE);
    for (const Record& record : records)
    {
        if (!encodeRecord(record, out)) return {};
    }
    return out;
}

Digest recordDigest(const Record& record)
{
    std::vector<uint8_t> canonical(1, record.type());
    putHeader(record, canonical);
    encodeBody(record.body, canonical, true);
    return digestOf(canonical);
}

DecodeResult decodeRecord(const uint8_t*& at, const uint8_t* end, Record& out)
{
    if (at >= end) return DecodeResult::Short;
    uint8_t type = at[0];
    if (type == 0) return DecodeResult::Invalid;
    size_t length = 0, fieldBytes = 0;
    switch (readLength(at + 1, end, length, fieldBytes))
    {
        case LengthRead::Short:
            return DecodeResult::Short;
        case LengthRead::Invalid:
            return DecodeResult::Invalid;
        case LengthRead::Ok:
            break;
    }
    const uint8_t* inner = at + 1 + fieldBytes;
    if ((size_t)(end - inner) < length) return DecodeResult::Short;
    const uint8_t* next = inner + length;
    at = next;

    Reader in(inner, next);
    Record record;
    if (!in.callsign(record.header.callsign) || !in.u24(record.header.seq) || !in.u32(record.header.time))
    {
        return DecodeResult::Malformed;
    }
    DecodeResult result = decodeBody(type, in, record);
    if (result == DecodeResult::Ok) out = std::move(record);
    return result;
}

bool decodeFrame(const uint8_t* bytes, size_t length, std::vector<Record>& out)
{
    if (length < 1 || bytes[0] != FRAME_BYTE) return false;
    const uint8_t* at = bytes + 1;
    const uint8_t* end = bytes + length;
    while (at < end)
    {
        Record record;
        switch (decodeRecord(at, end, record))
        {
            case DecodeResult::Ok:
                out.push_back(std::move(record));
                break;
            case DecodeResult::Malformed:
                break;
            case DecodeResult::Short:
            case DecodeResult::Invalid:
                return false;
        }
    }
    return true;
}

} // namespace protocol
} // namespace ragtime
