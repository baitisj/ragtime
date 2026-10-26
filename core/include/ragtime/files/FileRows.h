// What the Files tab shows (docs/PLAN.md, "The Files tab"): a row for
// every transfer that isn't finished, and a lasting log of the finished
// ones. Session files (package G) and broadcast files (package H) both
// fill it: each hands the tab its rows in progress as FileRow, and adds a
// FinishedFile to the one FileHistory when a transfer ends.
//
// The history is kept across restarts as plain text (saveFilesText,
// loadFilesText), with the session's part files kept beside it; a line of
// a kind a reader doesn't know is kept as it is, so package H can keep its
// own lines in the same file.
#ifndef RAGTIME_FILES_FILE_ROWS_H
#define RAGTIME_FILES_FILE_ROWS_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ragtime
{
namespace files
{

// Where a transfer happens.
enum class Via
{
    Session,    // in a ragchew, to or from the partner
    Broadcast,  // on the D2G group, with seal-driven repair
};

// "Broadcast": the receiver in the de column for a file we broadcast.
extern const char* const BROADCAST_PEER;

// A broadcast file's pieces in FileRow::pieces.
constexpr uint8_t PIECE_WAITING = 0;    // not yet sent or heard
constexpr uint8_t PIECE_HELD = 1;
constexpr uint8_t PIECE_MISSED = 2;     // its turn went by without it
// Missing, but a repair (parity) piece held stands in for it: it is
// rebuilt once as many repair pieces as are missing have come.
constexpr uint8_t PIECE_COVERED = 3;

// One row of "In progress and parts".
struct FileRow
{
    // How the state reads: the tab colours a row by it, never by colour
    // alone (the state word is always there too).
    enum class Look
    {
        Waiting,    // queued, offered, or asking us
        Moving,
        Stalled,    // moving, but nothing for a while
        Repairing,  // a broadcast share taking repair requests
        Kept,       // a part kept from before
    };

    Via via = Via::Session;
    uint64_t id = 0;            // the source's own, to name the row by in its menu
    uint64_t atMs = 0;          // the date column: when it was offered, queued or kept (Unix ms)
    std::string name;
    uint64_t size = 0;
    uint64_t done = 0;          // the bar's dark shade: acknowledged (sending) or written (receiving)
    uint64_t handed = 0;        // the light shade: sent but not yet acknowledged, or still arriving
    bool outgoing = true;
    std::string peer;           // the other station, or BROADCAST_PEER
    std::string state;          // "Waiting for answer", "Moving", "Stalled 1:20", "Repairing", "Part kept"
    Look look = Look::Waiting;
    uint64_t expiresAtMs = 0;   // when a kept part goes, an offer expires, or repairs end; 0: never
    double bytesPerSecond = 0;  // over the last 2 minutes; 0 while not known
    uint64_t finishAtMs = 0;    // one estimate of when it ends; 0 while not known
    // A broadcast file's pieces, for a segmented bar: PIECE_WAITING,
    // PIECE_HELD, PIECE_MISSED or PIECE_COVERED each; empty for a session
    // file.
    std::vector<uint8_t> pieces;
};

// A finished transfer, as the Completed pane and its log keep it.
struct FinishedFile
{
    enum class Result
    {
        Saved,      // received and saved
        Sent,       // the far end saved it, or a broadcast went out
        Declined,
        Cancelled,  // by this operator
        CancelledThere,
        Expired,    // the offer went unanswered
        NotTaken,   // the far end takes no files
        Failed,
    };

    uint64_t id = 0;            // given by FileHistory::add; not kept across restarts
    Via via = Via::Session;
    uint64_t atMs = 0;          // when it finished (Unix ms)
    bool outgoing = true;
    std::string peer;           // the other station, or BROADCAST_PEER
    std::string name;
    uint64_t size = 0;
    Result result = Result::Saved;
    std::string reason;         // why it failed, when known
    std::string path;           // where it was saved, or the file that was sent (UTF-8)
    // For the bps column: the bytes that crossed and how long they took,
    // first byte to last; 0 when not known (not saved or sent, or a log
    // from before 2026-10-22).
    uint64_t movedBytes = 0;
    uint64_t movedMs = 0;
};

// "Saved", "Sent", "Declined", "Cancelled", "Cancelled by W1AW", "Expired",
// "Not taken", "Failed: <reason>".
std::string resultLabel(const FinishedFile& file);
// The bps column: bits a second over the whole transfer, movedBytes × 8
// / movedMs, as "236" or "1,180"; "" when unknown or under a second.
std::string bpsText(const FinishedFile& file);

// The Completed pane's log, newest last. Not thread safe.
class FileHistory
{
public:
    // The oldest go past this many.
    static constexpr size_t MAX_ENTRIES = 500;

    // Adds it with a fresh id, which it returns.
    uint64_t add(FinishedFile file);
    bool remove(uint64_t id);
    void clear();
    const FinishedFile* find(uint64_t id) const;
    const std::deque<FinishedFile>& entries() const { return entries_; }
    // Moves with every change.
    uint64_t version() const { return version_; }

private:
    std::deque<FinishedFile> entries_;
    uint64_t nextId_ = 1;
    uint64_t version_ = 0;
};

// A part file kept from a session transfer that ended unfinished, so the
// same file from the same station carries on from it, across restarts.
// Or, outgoing, the other end of one: a file of ours that ended unfinished
// with part of it sent, which the far end keeps as its part, so "Send the
// rest" carries on from there.
struct KeptPart
{
    uint64_t id = 0;            // SessionFiles' own, for the row's menu; not kept across restarts
    bool outgoing = false;      // ours, part sent; else theirs, part kept here
    std::string peer;
    std::string name;           // the name offered, made safe
    uint64_t size = 0;          // the whole file's
    uint8_t digest[12] = {};    // the whole file's (protocol::Digest)
    std::string path;           // where it was to be saved, or the file sent (UTF-8)
    std::string part;           // the part file (UTF-8); empty for one sent
    uint64_t bytes = 0;         // how much of the file it holds, or the far end's modem acknowledged
    // Outgoing: handed to the host, which sends what it has though the
    // transfer was cancelled; more than bytes while that still goes out.
    // Not in the log: it is bytes again after a restart.
    uint64_t handed = 0;
    uint64_t keptAtMs = 0;
    uint64_t keepUntilMs = 0;   // when it is deleted
    // How the transfer that left it ended ("Cancelled by KC2G"). The file
    // isn't finished while its part is kept, so this goes into Completed
    // only when the part goes without being carried on: deleted, or past
    // its time. hasEnding is false for a part from a log written before
    // 2026-10-23 whose ending is already in Completed.
    bool hasEnding = false;
    FinishedFile ending;
};

// The history and the kept parts as text, one line each, and back.
// Lines of kinds this reader doesn't know come back in other, so a
// reader that writes the file again keeps them.
std::string saveFilesText(const FileHistory& history, const std::vector<KeptPart>& kept,
                          const std::vector<std::string>& other = {});
// False if the text isn't a Ragtime files log at all; bad lines are skipped.
bool loadFilesText(const std::string& text, FileHistory& history, std::vector<KeptPart>& kept,
                   std::vector<std::string>* other = nullptr);

//-------------------------------------------------------------------------
// Words and numbers for the tab
//-------------------------------------------------------------------------

// 7000 -> "7,000".
std::string groupedNumber(uint64_t n);
// A kept part's state in the tab: "Part kept, cancelled by AG7EW" (or
// just "Part kept"); one sent: "Part sent, cancelled", or "Stopping,
// 1,756 bytes still going out" while the host sends what it was handed.
std::string partStateText(const KeptPart& part);
// "7,000 bytes", "1 byte"; "6.8 KB" from 10,000 up, "1.2 MB" from 1,000,000.
std::string sizeText(uint64_t bytes);
// "4,096 of 7,000".
std::string progressText(uint64_t done, uint64_t size);
// "38 B/s", "1.2 KB/s"; "" for 0.
std::string rateText(double bytesPerSecond);
// "ETA 14:32", "ETA Oct 29" a day or more off; "" for 0. utcOffsetS gives
// local time.
std::string finishText(uint64_t finishAtMs, uint64_t nowMs, long utcOffsetS);
// A row's state with its finish estimate: "QRL ETA 14:32",
// "Receiving, ETA 14:32", or the state alone when there is no estimate.
std::string stateText(const FileRow& row, uint64_t nowMs, long utcOffsetS);
// When something ends: "3:10" while under an hour away (a countdown),
// "15:42 (1 h 10 min)" today, "Oct 29" further off; "" for 0.
std::string expiryText(uint64_t atMs, uint64_t nowMs, long utcOffsetS);
// The date column: "14:32" today, "Oct 21 14:32" before.
std::string dateText(uint64_t atMs, uint64_t nowMs, long utcOffsetS);
// The de column, sender → receiver with the operator as OM: "OM → W1AW",
// "W1AW → OM", "OM → Broadcast".
std::string peerText(bool outgoing, const std::string& peer);

// The transfer rate over a sliding window, from how far a transfer has
// got now and then. The rate is over the last WINDOW_MS, or since it
// started moving if that is shorter.
class RateMeter
{
public:
    static constexpr uint64_t WINDOW_MS = 2 * 60 * 1000;

    // How far it has got (bytes) at nowMs.
    void sample(uint64_t done, uint64_t nowMs);
    void reset();
    // Bytes a second; 0 until it has moved for a few seconds.
    double rate() const;
    // When the rest would be in at this rate; 0 when unknown.
    uint64_t finishAtMs(uint64_t size, uint64_t nowMs) const;
    // When it last moved; 0 never.
    uint64_t lastMovedMs() const { return lastMovedMs_; }

private:
    struct Sample
    {
        uint64_t atMs;
        uint64_t done;
    };
    std::deque<Sample> samples_;
    uint64_t lastMovedMs_ = 0;
};

} // namespace files
} // namespace ragtime

#endif
