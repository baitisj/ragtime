// Our side of a session's byte stream, kept record by record from the
// first record not yet acknowledged, so a cut can be mended.
//
// A cut (CLEANTXBUFFER, Data2G PR #118) drops what data2g-host holds of
// our stream and hasn't sent. The host cuts at its own record boundaries,
// not ours, so the far end may have the start of one of our records and
// not its end: that record's tail goes again, from this log, before
// anything else, and the far end's reader never sees a seam. The records
// that started after the cut are handed back, to be written again or let
// go (docs/PROTOCOL.md, "Cuts").
#ifndef RAGTIME_SESSION_STREAM_LOG_H
#define RAGTIME_SESSION_STREAM_LOG_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ragtime
{
namespace session
{

class StreamLog
{
public:
    // One record of ours (or the stream's opening byte, type 0), where it
    // sits in our stream, and the message it carries (0 for none).
    struct Span
    {
        uint64_t start = 0;
        uint64_t end = 0;
        uint8_t type = 0;
        uint64_t message = 0;
    };

    // What a cut left: the bytes that finish the record the cut fell in
    // (empty when it fell between records), and the records that started
    // at or after it, with their bytes.
    struct Cut
    {
        std::vector<uint8_t> tail;
        std::vector<Span> lost;
        std::vector<std::vector<uint8_t>> lostBytes;
    };

    // Bytes written at end(): whole records, and first the stream's
    // opening byte. message names the record the bytes start (a Text).
    void wrote(const uint8_t* bytes, size_t length, uint64_t message = 0);
    void wrote(const std::vector<uint8_t>& bytes, uint64_t message = 0)
    {
        wrote(bytes.data(), bytes.size(), message);
    }
    // The far end's modem has acknowledged our stream up to here: records
    // wholly before it are let go.
    void acknowledged(uint64_t acked);
    void reset();

    uint64_t start() const { return base_; }
    uint64_t end() const { return base_ + bytes_.size(); }
    const std::deque<Span>& spans() const { return spans_; }

    // Our stream from c on never went. False if the log no longer holds c
    // (the record it falls in was let go) or c is past the end. After it
    // the log ends at c; the tail, written next, finishes that record.
    bool cut(uint64_t c, Cut& out);

private:
    void parse(uint64_t message);

    uint64_t base_ = 0;             // the stream offset of bytes_[0]
    std::deque<uint8_t> bytes_;
    std::deque<Span> spans_;        // the last may end past end(): its tail is still to come
};

} // namespace session
} // namespace ragtime

#endif
