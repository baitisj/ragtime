// Session files riding in a ragchew (Stage 1 package G): SessionFiles
// plugged into SessionChat as its SessionTraffic.
//
// - Offers wait for the far end's Hello, and go only if it takes session
//   files (docs/PROTOCOL.md, Hello); files queued for a station that
//   doesn't end Not taken.
// - File bytes are paced: no more of them go to data2g-host than about a
//   minute of the session's acknowledged rate (WINDOW_MS), between
//   MIN_WINDOW and MAX_WINDOW bytes waiting, so a message typed during a
//   transfer isn't stuck behind the whole file.
// - Leave cancels what is under way in the session, so the far end hears
//   it, and keeps the files still queued for the next session with the
//   station. A part received is kept, to carry on next time.
// - Each transfer's rate over the last 2 minutes, one finish estimate, and
//   Stalled once nothing has moved for STALL_MS.
// - Transfers that end go to the Files tab's lasting log (FileHistory).
//
// No sockets and no clock: tick() is called every second.
#ifndef RAGTIME_FILES_FILE_TRAFFIC_H
#define RAGTIME_FILES_FILE_TRAFFIC_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ragtime/files/FileRows.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/session/SessionChat.h"

namespace ragtime
{
namespace files
{

class FileTraffic : public session::SessionTraffic
{
public:
    // File bytes waiting at the host, unacknowledged: this much of the
    // session's rate, within these bounds, and START_WINDOW until the rate
    // is known.
    static constexpr uint64_t WINDOW_MS = 60 * 1000;
    static constexpr uint64_t MIN_WINDOW = 1024;
    static constexpr uint64_t START_WINDOW = 4096;
    static constexpr uint64_t MAX_WINDOW = 16384;
    // A FileData carries at most half the window, and at least this much.
    static constexpr size_t MIN_PIECE = 256;
    // Moving, but nothing for this long: Stalled.
    static constexpr uint64_t STALL_MS = 30 * 1000;

    struct Event
    {
        enum class Type
        {
            Offered,    // a file offered to us (autoAccepted: taken without asking)
            Finished,   // a transfer ended; it is in the history as historyId
        } type = Type::Offered;
        uint64_t id = 0;
        std::string peer;
        bool autoAccepted = false;
        uint64_t historyId = 0;
    };

    FileTraffic(SessionFiles& files, FileHistory& history);

    SessionFiles& files() { return files_; }
    const SessionFiles& files() const { return files_; }

    // SessionTraffic.
    void greeted(const std::string& peer, const protocol::Hello& hello) override;
    void onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs) override;
    void arriving(const std::string& peer, uint32_t offer, size_t bytes) override;
    void write(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset, uint64_t nowMs,
               std::vector<uint8_t>& out) override;
    void acknowledged(const std::string& peer, uint64_t streamAcked) override;
    void leaving(const std::string& peer, protocol::Numbering& numbering, uint64_t nowMs,
                 std::vector<uint8_t>& out) override;
    void ended(const std::string& peer, uint64_t nowMs) override;
    // A file is offered, being answered or moving with the peer.
    bool busy(const std::string& peer) const override;

    // Every second: offers expire, kept parts go, rates are sampled.
    void tick(uint64_t nowMs);
    // After the operator acts on a file (offer, accept, cancel...): what
    // that ended goes to the history.
    void update(uint64_t nowMs);
    std::vector<Event> takeEvents();

    // A session is up with the peer and its Hello says it takes files.
    bool takesFiles(const std::string& peer) const;
    // How many bytes may wait at the host now.
    uint64_t window() const;

    // The Files tab's rows: every transfer still going, and the kept parts.
    std::vector<FileRow> rows(uint64_t nowMs) const;
    // One transfer's row (for its card in the Session tab).
    FileRow row(const FileTransfer& t, uint64_t nowMs) const;
    // Moves whenever rows() may have changed apart from the clock.
    uint64_t changes() const { return files_.changes() + changes_; }

private:
    void collect(uint64_t nowMs);
    void sample(uint64_t nowMs);
    std::string stateText(const FileTransfer& t, uint64_t nowMs, FileRow::Look& look) const;

    SessionFiles& files_;
    FileHistory& history_;
    std::string peer_;          // the session's, the last one up
    bool up_ = false;           // a session with peer_ is up
    bool greeted_ = false;
    bool takes_ = false;
    uint64_t acked_ = 0;        // our stream, acknowledged
    RateMeter session_;         // the session's acknowledged rate
    std::map<uint64_t, RateMeter> meters_;  // live transfers', by id
    std::set<uint64_t> recorded_;           // finished, in the history
    std::set<uint64_t> announced_;          // offered to us, told
    std::vector<Event> events_;
    uint64_t seenChanges_ = ~0ull;
    uint64_t changes_ = 0;
};

} // namespace files
} // namespace ragtime

#endif
