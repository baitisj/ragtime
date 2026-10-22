// Numbering a station's records, and reading a session's byte stream
// (docs/PROTOCOL.md, "Sequence numbers" and "Sessions").
#ifndef RAGTIME_PROTOCOL_STREAM_H
#define RAGTIME_PROTOCOL_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace protocol
{

// Gives a stream's records their headers. A station has one Numbering for
// its broadcasts, kept across restarts, and a new one for each session.
class Numbering
{
public:
    explicit Numbering(std::string callsign = std::string(), uint32_t newestItem = 0)
        : callsign_(std::move(callsign)), newest_(newestItem)
    {
    }

    void setCallsign(const std::string& callsign) { callsign_ = callsign; }
    const std::string& callsign() const { return callsign_; }

    // The newest item's number, 0 before the first.
    uint32_t newestItem() const { return newest_; }

    // A header for a record of this type: an item takes the next number,
    // a part of an item its item's (item), anything else the newest
    // item's. Numbers run 1 to 0xFFFFFF, then start again at 1.
    Header header(RecordType type, uint32_t time, uint32_t item = 0);

    // The record, with its header filled in.
    Record record(Body body, uint32_t time, uint32_t item = 0);

private:
    std::string callsign_;
    uint32_t newest_ = 0;
};

// A session's byte stream, as it arrives: FRAME_BYTE, then records, the
// first of them a Hello. A stream that doesn't start that way, or breaks
// its framing, isn't Ragtime's, and nothing more is taken from it.
class StreamReader
{
public:
    // Appends every whole record; false once the stream has shown it is
    // not ours. Malformed records are skipped (counted in malformed()).
    bool feed(const uint8_t* bytes, size_t length, std::vector<Record>& out);
    bool feed(const std::vector<uint8_t>& bytes, std::vector<Record>& out)
    {
        return feed(bytes.data(), bytes.size(), out);
    }

    void reset();

    bool foreign() const { return foreign_; }
    // The far end's Hello has come.
    bool greeted() const { return greeted_; }
    const Hello& hello() const { return hello_; }
    const std::string& peer() const { return peer_; }
    size_t malformed() const { return malformed_; }

    // A FileData record partly in: its offer and how many file bytes have
    // come so far; false when none is.
    bool partialFileData(uint32_t& offer, size_t& bytes) const;

private:
    bool notOurs();

    std::vector<uint8_t> pending_;
    bool opened_ = false;       // FRAME_BYTE seen
    bool greeted_ = false;
    bool foreign_ = false;
    Hello hello_;
    std::string peer_;
    size_t malformed_ = 0;
};

// What a session's sender writes first: FRAME_BYTE and its Hello.
std::vector<uint8_t> streamOpening(const Record& hello);

} // namespace protocol
} // namespace ragtime

#endif
