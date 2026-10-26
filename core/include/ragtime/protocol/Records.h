// Ragtime's records, protocol v0 (docs/PROTOCOL.md): what each one holds,
// and how records go into a broadcast frame or a session's byte stream.
//
// Every record starts with its type and length, then the sender's
// callsign, a sequence number and the time, so a listener who fades in
// mid-stream knows who it is. No sockets here, and no clock: the time is
// whatever the caller puts in the header.
#ifndef RAGTIME_PROTOCOL_RECORDS_H
#define RAGTIME_PROTOCOL_RECORDS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ragtime/protocol/Fields.h"

namespace ragtime
{
namespace protocol
{

constexpr int VERSION = 0;

// A frame, and a session's byte stream, open with this byte: 0xA in the
// top nibble for Ragtime, the protocol version in the bottom one. It also
// means no frame is ever all zeros.
constexpr uint8_t FRAME_BYTE = 0xA0 | VERSION;

// The longest frame Ragtime puts on the D2G group (v0). Glissando's frames
// were this size on the air; larger ones wait on a check with Data2G.
constexpr size_t MAX_BROADCAST_FRAME = 255;

enum class RecordType : uint8_t
{
    Hello = 0x01,
    Status = 0x02,
    Text = 0x10,
    Ack = 0x11,
    Ping = 0x12,
    Pong = 0x13,
    FileOffer = 0x20,
    FileAnswer = 0x21,
    FileData = 0x22,
    FileSaved = 0x23,
    FileCancel = 0x24,
    FileAnnounce = 0x30,
    FilePiece = 0x31,
    Seal = 0x40,
    // 0x41 was 0.1's RepairRequest, with lists of pieces; it is skipped
    // now as a type unknown, so a 0.1 station still chats with us.
    RepairRefused = 0x42,
    RepairRequest = 0x43,
    ItemParity = 0x44,
};

// Which header sequence number a record carries (docs/PROTOCOL.md,
// "Sequence numbers"): items take the next number, a record that carries
// part of an item has its item's, and the rest the newest item's.
bool isItem(RecordType type);
bool isPartOfItem(RecordType type);

struct Header
{
    std::string callsign;
    uint32_t seq = 0;       // 24 bits
    uint32_t time = 0;      // Unix seconds, UTC
};

// The records each end takes, in a Hello.
enum Takes : uint8_t
{
    TAKES_TEXT = 0x01,              // Text and Ack
    TAKES_STATUS = 0x02,
    TAKES_SESSION_FILES = 0x04,     // FileOffer .. FileCancel
    TAKES_BROADCAST_FILES = 0x08,   // FileAnnounce, FilePiece
    TAKES_SEALS = 0x10,             // Seal, RepairRequest, RepairRefused, ItemParity
    TAKES_PINGS = 0x20,             // Ping and Pong (Ragtime 0.1.2 on)
    TAKES_ALL_V0 = 0x3F,
};

struct Hello
{
    uint8_t version = VERSION;
    bool acceptsSessions = false;
    bool automatic = false;         // nobody at the controls (Stage 2 gives it its rules)
    uint8_t takes = TAKES_ALL_V0;
    std::string locator;            // 4 or 6 characters, or empty
    std::string software;           // "Ragtime 0.1.0", or empty
};

struct Status
{
    enum class State : uint8_t
    {
        Qrv = 0,    // available
        Qrl = 1,    // in a session (with partner, when named)
        Qrt = 2,    // closing down
    };
    State state = State::Qrv;
    std::string partner;
};

// How a Text's characters are coded: the low nibble of its flags.
constexpr uint8_t CODING_TABLE_0 = 0;   // HamText, text table 0
constexpr uint8_t CODING_RAW = 15;      // the UTF-8 bytes as they are

struct Text
{
    std::string text;           // the exact UTF-8 bytes typed
    std::string to;             // addressed to one station, or empty
    bool ackWanted = false;     // only with to: the station answers with an Ack
    // Decoded: how it was coded. Encoded: left alone, the shorter of
    // table 0 and raw is chosen.
    uint8_t coding = CODING_TABLE_0;
};

struct Ack
{
    std::string to;             // who sent the Text
    uint32_t item = 0;          // its sequence number
};

// A station asking another how well it hears us: the station answers with
// a Pong, by itself, in the mode named (docs/PROTOCOL.md, "Pings").
struct Ping
{
    std::string to;
    uint16_t number = 0;        // the sender's own count, so a Pong names its Ping
    std::string mode;           // the Data2G mode it went in; empty for the group's
};

// The answer to a Ping: the S/N the Ping was heard at.
struct Pong
{
    std::string to;             // who sent the Ping
    uint16_t number = 0;        // the Ping's
    std::optional<double> snr;  // as repairSnrCode codes it, in a byte of its own; none without a figure
};

constexpr size_t MAX_FILE_NAME = 200;

struct FileOffer
{
    uint32_t size = 0;
    Digest digest{};            // of the whole file
    std::string name;           // UTF-8, at most MAX_FILE_NAME bytes
};

struct FileAnswer
{
    uint32_t offer = 0;         // the offer's sequence number
    bool accept = false;
    uint32_t from = 0;          // accepted: the bytes already held, so only the rest comes
};

// The header's seq is the offer's.
struct FileData
{
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
};

struct FileSaved
{
    uint32_t offer = 0;
};

enum class CancelReason : uint8_t
{
    SenderStopped = 0x01,       // the sending operator cancelled it
    SenderFailed = 0x02,        // the file could not be read
    SenderExpired = 0x03,       // the offer went unanswered
    ReceiverStopped = 0x81,     // the receiving operator cancelled it
    ReceiverFailed = 0x82,      // the file could not be written, or did not check
    ReceiverExpired = 0x83,
};
constexpr uint8_t CANCEL_FROM_RECEIVER = 0x80;

struct FileCancel
{
    uint32_t offer = 0;
    uint8_t reason = 0;         // a CancelReason
};

struct FileAnnounce
{
    uint32_t size = 0;
    uint16_t pieceBytes = 0;
    Digest digest{};
    std::string name;
};

// The header's seq is the announce's. An index at or past the file's
// piece count names a parity piece (docs/PROTOCOL.md, "Parity"): the
// pieces are the data symbols, named by their indexes, and this is the
// parity symbol named by its own, files::parity::symbolLength(piece size)
// bytes long.
struct FilePiece
{
    uint16_t index = 0;
    std::vector<uint8_t> bytes;
};

struct Seal
{
    uint16_t number = 0;
    Digest previous{};          // the last seal's digest; zeros before the first
    uint32_t firstItem = 0;
    std::vector<Digest> items;  // firstItem, firstItem + 1, ...
};

struct RepairRequest
{
    std::string to;             // whose seal
    uint16_t seal = 0;
    Digest sealDigest{};        // zeros when asking for a seal not heard
    bool wantSeal = false;      // send the seal itself again
    // The S/N the asker measured on the seal's sender, the lowest of its
    // recent bursts, so repairs go in a gear the asker can copy; none when
    // it has no figure. It rides in the flags byte's bits 1 to 7
    // (repairSnrCode).
    std::optional<double> snr;
    std::vector<bool> missingItems;     // one per item in the seal
    // How many repair symbols for those items the asker wants: as many as
    // it lacks, and more for what it expects to lose (files::parity::wantFor).
    uint8_t itemsWant = 0;
    // For each file in the seal it lacks pieces of: the file's sequence
    // number, and how many parity pieces it wants.
    std::vector<std::pair<uint32_t, uint16_t>> files;
};

// A RepairRequest's S/N as its 7-bit code: 0 for none, else 1 to 127 for
// REPAIR_SNR_LOWEST_DB to REPAIR_SNR_LOWEST_DB + 63 dB in half-dB steps,
// rounded to the nearest and held at both ends.
constexpr double REPAIR_SNR_LOWEST_DB = -24.0;
constexpr double REPAIR_SNR_STEP_DB = 0.5;
uint8_t repairSnrCode(std::optional<double> dB);
std::optional<double> repairSnr(uint8_t code);

// Why a seal's sender won't repair what was asked for.
enum class RefusalReason : uint8_t
{
    StoppedSharing = 0,     // the operator closed its repairs
    Cancelled = 1,          // the operator cancelled the file
    NotHeld = 2,            // the sender no longer holds it (expired, or lost in a restart)
    Closed = 3,             // the seal is out of its sensitivity window
};

// The seal's sender saying no, once for every station that asked: the
// reason holds for these files under the seal, or for everything under it
// (files empty).
struct RepairRefused
{
    uint16_t seal = 0;
    uint8_t reason = 0;         // a RefusalReason; another is a refusal all the same
    std::vector<uint32_t> files;
};

// Parity over items a seal lists (docs/PROTOCOL.md, "Parity"): the
// items it covers, each as its encoded record padded with zeros to the
// length of bytes, are the data symbols, named by their places in the
// seal; this is the parity symbol named ITEM_PARITY_BASE + index. A
// listener holding all but k of the items covered rebuilds them from any
// k of these over the same items.
constexpr uint16_t ITEM_PARITY_BASE = 256;

struct ItemParity
{
    uint16_t seal = 0;
    std::vector<bool> items;    // the items it covers, one per item in the seal
    uint16_t index = 0;
    std::vector<uint8_t> bytes;
};

// A type this version doesn't know: kept as it came, so it can be passed
// on or shown.
struct Unknown
{
    uint8_t type = 0;
    std::vector<uint8_t> body;
};

using Body = std::variant<Unknown, Hello, Status, Text, Ack, FileOffer, FileAnswer, FileData, FileSaved,
                          FileCancel, FileAnnounce, FilePiece, Seal, RepairRequest, RepairRefused, ItemParity,
                          Ping, Pong>;

struct Record
{
    Header header;
    Body body;

    uint8_t type() const;
    template <typename T>
    const T* as() const { return std::get_if<T>(&body); }
    template <typename T>
    T* as() { return std::get_if<T>(&body); }
};

//-------------------------------------------------------------------------
// Encoding
//-------------------------------------------------------------------------

// Appends the record. False, with nothing appended, if a field can't be
// coded: a callsign that isn't one, a name too long, a record over
// MAX_RECORD_LENGTH.
bool encodeRecord(const Record& record, std::vector<uint8_t>& out);
std::vector<uint8_t> encodeRecord(const Record& record);

// FRAME_BYTE and the records; empty if one can't be coded.
std::vector<uint8_t> encodeFrame(const std::vector<Record>& records);

// The record's digest (docs/PROTOCOL.md, "Digests"): SHA-256 of its
// canonical form, first 12 bytes. A Text's canonical form has its UTF-8,
// never the coded bits, so a new text table never changes a digest.
Digest recordDigest(const Record& record);

//-------------------------------------------------------------------------
// Decoding
//-------------------------------------------------------------------------

enum class DecodeResult
{
    Ok,
    Short,          // the record is not all here yet (a stream still arriving)
    Malformed,      // its length is right, its body isn't: skipped
    Invalid,        // type 0 or a bad length: nothing after it can be trusted
};

// Decodes the record at [at, end), moving at past it unless Short or
// Invalid. A type this version doesn't know is Ok, as Unknown.
DecodeResult decodeRecord(const uint8_t*& at, const uint8_t* end, Record& out);

// A broadcast frame's records. False if it doesn't open with FRAME_BYTE
// or breaks off; the records before the break are still given.
bool decodeFrame(const uint8_t* bytes, size_t length, std::vector<Record>& out);
inline bool decodeFrame(const std::vector<uint8_t>& frame, std::vector<Record>& out)
{
    return decodeFrame(frame.data(), frame.size(), out);
}

} // namespace protocol
} // namespace ragtime

#endif
