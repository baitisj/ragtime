// Session files riding in a ragchew (Stage 1 package G): SessionFiles
// plugged into SessionChat as its SessionTraffic.
//
// - Offers wait for the far end's Hello, and go only if it takes session
//   files (docs/PROTOCOL.md, Hello); files queued for a station that
//   doesn't end Not taken.
// - File bytes are paced: no more of them wait at data2g-host than two of
//   the session mode's longest bursts (WINDOW_BURSTS), between MIN_WINDOW
//   and MAX_WINDOW bytes, so the host always has the next burst queued,
//   yet a message typed during a transfer isn't stuck behind the whole
//   file, and a cancel isn't either: data2g-host can't take back what it
//   has been handed. Until the mode is known, 10 s of the session's
//   acknowledged rate (WINDOW_MS). A host that can cut (DATA2G ON, Data2G
//   PR #118) takes back what it hasn't sent, so there the window is
//   MAX_WINDOW from the start, and a file stopped either end is cut.
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
#include "ragtime/link/Data2GLink.h"
#include "ragtime/session/SessionChat.h"

namespace ragtime
{
namespace files
{

class FileTraffic : public session::SessionTraffic
{
public:
    // File bytes waiting at the host, unacknowledged: two of the session
    // mode's longest bursts, within these bounds. A burst on a session
    // runs at most 24 s on FSK and 12 s on OFDM (Andrew, 2026-10-24), and
    // a host with less than its next burst queued idles into CSMA between
    // bursts, which starves the fast modes (Andrew, 2026-10-25). The cap
    // is what Andrew asked to keep queued, 6 to 8 KB; a cancel waits
    // behind it, until data2g-host can drop what it holds (Data2G #118).
    static constexpr uint64_t WINDOW_BURSTS = 2;
    static constexpr double FSK_BURST_SECONDS = 24.0;
    static constexpr double OFDM_BURST_SECONDS = 12.0;
    static constexpr uint64_t MIN_WINDOW = 1024;
    static constexpr uint64_t MAX_WINDOW = 8192;
    // With the mode not yet known (no MODE report yet, or a mode the host
    // didn't list): this much of the session's acknowledged rate, and
    // START_WINDOW until the rate is known.
    static constexpr uint64_t WINDOW_MS = 10 * 1000;
    static constexpr uint64_t START_WINDOW = MIN_WINDOW;
    // Where the host can cut, a cut falls inside one FileData and that
    // record's tail still goes: a FileData carries at most half a burst of
    // the mode, between MIN_PIECE and SessionFiles::PIECE_BYTES, or
    // CUT_PIECE while the mode isn't known.
    static constexpr size_t CUT_PIECE = 512;
    // A FileData carries at most half the window, and at least this much.
    static constexpr size_t MIN_PIECE = 256;
    // Moving, but nothing for this long: Stalled.
    static constexpr uint64_t STALL_MS = 30 * 1000;

    struct Event
    {
        enum class Type
        {
            Offered,    // a file offered to us (autoAccepted: taken without asking)
            Finished,   // a transfer ended, as finished says
        } type = Type::Offered;
        uint64_t id = 0;
        std::string peer;
        bool autoAccepted = false;
        // Finished: how it ended. It is in the history as historyId, or,
        // when partKept, not yet: the file isn't done while its part is
        // kept, and goes into the history when the part goes unused.
        FinishedFile finished;
        bool partKept = false;
        uint64_t historyId = 0;
    };

    FileTraffic(SessionFiles& files, FileHistory& history);

    SessionFiles& files() { return files_; }
    const SessionFiles& files() const { return files_; }

    // SessionTraffic.
    void greeted(const std::string& peer, const protocol::Hello& hello) override;
    void onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs) override;
    void arriving(const std::string& peer, uint32_t offer, uint32_t offset, const uint8_t* data,
                  size_t bytes) override;
    void write(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset, uint64_t nowMs,
               std::vector<uint8_t>& out) override;
    void acknowledged(const std::string& peer, uint64_t streamAcked) override;
    void leaving(const std::string& peer, protocol::Numbering& numbering, uint64_t nowMs,
                 std::vector<uint8_t>& out) override;
    void ended(const std::string& peer, uint64_t nowMs) override;
    bool wantsCut(const std::string& peer) override;
    void cut(const std::string& peer, uint64_t streamFrom) override;
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
    // The session's mode, as data2g-host last reported it (invalid when
    // not known).
    void setMode(const link::ModeInfo& mode) { mode_ = mode; }
    const link::ModeInfo& mode() const { return mode_; }
    // The session's acknowledged rate over the last 2 minutes, bytes a
    // second; 0 while not known.
    double sessionRate() const { return session_.rate(); }
    // The host can cut what it holds (it took DATA2G ON).
    void setCuts(bool cuts) { cuts_ = cuts; }
    bool cuts() const { return cuts_; }
    // How many bytes may wait at the host now, and the most of the file
    // one FileData carries.
    uint64_t window() const;
    size_t pieceBytes() const;
    // The most one burst in this mode carries in a session (0 for an
    // invalid mode).
    static uint64_t burstBytes(const link::ModeInfo& mode);

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
    link::ModeInfo mode_;       // the session's mode, if known
    bool cuts_ = false;
    uint64_t seenCuts_ = 0;     // files_.cutsWanted() last seen
    std::map<uint64_t, RateMeter> meters_;  // live transfers', by id
    struct Start
    {
        uint64_t atMs;
        uint64_t done;
    };
    std::map<uint64_t, Start> started_;     // when each first moved, and from where
    std::set<uint64_t> recorded_;           // finished, in the history
    std::set<uint64_t> announced_;          // offered to us, told
    std::vector<Event> events_;
    uint64_t seenChanges_ = ~0ull;
    uint64_t changes_ = 0;
};

} // namespace files
} // namespace ragtime

#endif
