// Files sent and received in a session (a ragchew): the offer, the answer,
// the file's bytes, the receiver's word that it has saved it, cancelling
// and expiry, and carrying on from a part kept from before. No sockets:
// the session's owner hands it the records that arrive and writes the
// bytes it gives back, so all of it is tested on its own.
//
// Ported from Glissando's Data2GFileTransfer (written there for Glissando)
// and relicensed MIT by Jeff, then moved onto Ragtime's records
// (docs/PROTOCOL.md, "Files in a session"): a transfer is named by its
// FileOffer's sequence number, every FileData says where in the file it
// starts, and the offer carries the whole file's digest, which the
// receiver checks before it saves.
#ifndef RAGTIME_FILES_SESSION_FILES_H
#define RAGTIME_FILES_SESSION_FILES_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/files/FileRows.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"

namespace ragtime
{
namespace files
{

// One file going one way, as the screen shows it. nowMs, wherever it is
// passed, is Unix time in milliseconds: records are stamped with it.
struct FileTransfer
{
    enum class State
    {
        // Ours, being sent.
        Waiting,        // for a session with the station, or for the file ahead of it
        Offered,        // the offer is in the session; no answer yet
        Sending,        // accepted: the bytes are going, then the far end saves it
        Delivered,      // the far end has saved it
        Declined,

        // Theirs, being received.
        Asking,         // offered to us: the operator has yet to answer
        Receiving,
        Saved,

        // Either way.
        Cancelled,          // by this operator
        CancelledThere,     // by the far end's operator
        Expired,            // the offer went unanswered for OFFER_EXPIRY_MS
        Failed,             // the session ended under it, or this side could not read or write it
        FailedThere,        // the far end could not write or read it
        NotTaken,           // the far end's Hello says it takes no files
    };

    uint64_t id = 0;            // this run's own, unique, for the screen to name it by
    bool outgoing = true;
    std::string peer;
    std::string name;           // the name offered; one received is made safe to save under
    uint64_t size = 0;
    uint64_t done = 0;          // sending: bytes the far end's modem has acknowledged; receiving: bytes written
    uint64_t handed = 0;        // sending: bytes handed to the modem, acknowledged or not; receiving: bytes
                                // arrived, written or in the record still coming in
    uint64_t expiresInMs = 0;   // an offer still unanswered: how long before it expires (see transfers())
    State state = State::Waiting;
    std::string path;           // the file sent, or where the one received is saved; UTF-8
    std::string error;          // why it failed, when this side knows
    bool autoAccepted = false;  // taken without asking, from a station on the auto-accept list
    // Theirs: part of this file, from an earlier transfer from the same
    // station that ended unfinished, is kept on disk, this much of it,
    // meant for keptPath; accepted to keptPath, only the rest comes.
    uint64_t kept = 0;
    std::string keptPath;
    uint64_t resumedFrom = 0;   // either way: where in the file this transfer started
    // Theirs, ended unfinished: what came is kept (keptParts()), so the
    // file isn't finished yet; it may be carried on.
    bool partKept = false;
    uint64_t atMs = 0;          // ours: when queued, then when offered; theirs: when offered to us
    protocol::Digest digest{};  // the whole file's
    // An inline picture (protocol::FILE_PICTURE): shown among the messages
    // as it comes, any first part of it the whole picture, softer.
    bool picture = false;
    // A picture the receiver stopped with the stop sign once it had enough
    // (CancelReason::ReceiverHasEnough): theirs is Saved with done of it,
    // ours Delivered with done of it gone.
    bool enough = false;

    // Still going: waiting, offered, being answered or under way.
    bool live() const;
};

// A name offered by the far end, made safe to save under: only its last
// path part, without control characters, path separators or the
// characters Windows refuses, without leading or trailing dots and
// spaces, and not a Windows device name. "received-file" when nothing is
// left.
std::string safeFileName(const std::string& offered);

// UTF-8 paths both ways, so names outside ASCII survive on Windows too.
std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// A name in the folder that neither a file nor a part file has taken:
// the name given, or "name (2).ext" and so on; empty if none is free up
// to 999.
std::filesystem::path freeSavePath(const std::filesystem::path& folder, const std::string& name);

// The whole file's digest, from its first size bytes; false if it can't
// be read.
bool fileDigest(const std::filesystem::path& path, uint64_t size, protocol::Digest& digest);

// The transfers of every station there has been a session with. One file
// goes to a station at a time; the next waits for it. Not thread safe.
class SessionFiles
{
public:
    // An offer nobody answers is given up on, by both sides, after this.
    static constexpr uint64_t OFFER_EXPIRY_MS = 4 * 60 * 1000;

    // The FileOffer's size field, and Ragtime's cap on anything over HF.
    static constexpr uint64_t MAX_FILE_BYTES = 300 * 1024;

    // The most of the file one FileData carries.
    static constexpr size_t PIECE_BYTES = 4096;

    // The part file of one received from a station that ended unfinished
    // is kept, for that station to send the same file again, for this
    // long, and so many at most, the oldest going first. keptParts() and
    // restoreKept() carry them across restarts; a part not restored is
    // left on disk when the engine goes.
    static constexpr uint64_t KEPT_MS = 7ull * 24 * 60 * 60 * 1000;
    static constexpr size_t KEPT_MAX = 8;

    // Progress moves in steps this big, within a FileData as well as at
    // its end.
    static constexpr uint64_t PROGRESS_STEP = 256;

    SessionFiles() = default;
    ~SessionFiles();

    SessionFiles(const SessionFiles&) = delete;
    SessionFiles& operator=(const SessionFiles&) = delete;

    //---------------------------------------------------------------------
    // The operator's side
    //---------------------------------------------------------------------

    // Queues a file for a station: it is offered once a session with the
    // station is up and nothing else is going to it. 0, with the reason,
    // if the file can't be sent.
    uint64_t offer(const std::string& peer, const std::string& path, uint64_t nowMs, std::string& error,
                   bool picture = false);

    // Answers an offer: the file is written to path plus ".part" (or a
    // numbered part file, if that name is taken), and renamed to path once
    // all of it has come and its digest checks. Refused, the offer left
    // open, if another transfer is being saved to path. To the offer's
    // keptPath, it carries on in the part file kept from before, and only
    // the rest comes.
    bool accept(uint64_t id, const std::string& path, std::string& error);
    bool decline(uint64_t id);

    // Stops a transfer either way; the far end is told. An offer to us
    // not yet answered is declined.
    bool cancel(uint64_t id);

    // Files from these stations are taken without asking, into folder,
    // under the name offered (numbered when one is there already). An
    // empty folder or list turns it off.
    void setAutoAccept(const std::string& folder, const std::vector<std::string>& calls);
    // Pictures of at most maxBytes, from any station, are taken without
    // asking too, into folder, so they show as they come. An empty folder
    // turns it off.
    void setAutoPictures(const std::string& folder, uint64_t maxBytes);

    // A picture of theirs being received: the stop sign. What has come is
    // saved as the picture, softer than the whole, and the sender is told
    // (ReceiverHasEnough) so the rest needn't go. With nothing come yet it
    // is cancelled. False if it isn't a picture being received.
    bool enough(uint64_t id);
    // A picture's bytes as they stand: all of one of ours or one saved,
    // or what has come of one being received, from its start. False if
    // there are none.
    bool pictureBytes(uint64_t id, std::vector<uint8_t>& out) const;

    // nowMs, when given, fills in each unanswered offer's expiresInMs.
    std::vector<FileTransfer> transfers(uint64_t nowMs = 0) const;
    const FileTransfer* transfer(uint64_t id) const;

    // Moves a file of ours still waiting its turn up (delta < 0) or down
    // the station's queue. False if it isn't waiting or can't move.
    bool reorder(uint64_t id, int delta);

    // The part files kept, for the log, and back after a restart: a part
    // whose file is gone, isn't the size noted or is past its time is
    // dropped. Parts restored take fresh ids. Parts sent go with them
    // (KeptPart::outgoing): a file of ours that ended unfinished with
    // some of it sent stays here, not finished, until it is sent again,
    // forgotten (deleteKept) or past its time, like the far end's part.
    std::vector<KeptPart> keptParts() const;
    void restoreKept(const std::vector<KeptPart>& parts, uint64_t nowMs);
    // Keep 7 days longer (from now or from when it would have gone,
    // whichever is later), and Delete part.
    bool keepLonger(uint64_t keptId, uint64_t ms, uint64_t nowMs);
    bool deleteKept(uint64_t keptId);
    // How the transfer that left a part ended (by the transfer's id), to
    // hand back from takeEnded() if the part goes without being carried
    // on. A part carried on ends with the transfer that carries it on.
    void setKeptEnding(uint64_t transferId, const FinishedFile& ending);
    // The endings of parts gone since the last call (deleted, past their
    // time or over KEPT_MAX), for Completed. atMs is left for the caller.
    std::vector<FinishedFile> takeEnded();

    // Moves whenever anything transfers() shows changes.
    uint64_t changes() const { return changes_; }

    //---------------------------------------------------------------------
    // The session's side, for the station a session is up with
    //---------------------------------------------------------------------

    // The records to write into the session now: answers, cancels and
    // FileSaved, then (with offerNext) the offer of the next file once
    // none is going. numbering is the session's: an offer takes the next
    // item number.
    std::vector<uint8_t> takeRecords(const std::string& peer, protocol::Numbering& numbering, uint64_t nowMs,
                                     bool offerNext = true);

    // Appends the next FileData for the file going to the station, at most
    // maxBytes of the file, if any; streamOffset is where in the session's
    // byte stream it will start.
    bool takePiece(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset,
                   uint64_t nowMs, std::vector<uint8_t>& out, size_t maxBytes = PIECE_BYTES);

    // The far end's modem has acknowledged the session's bytes up to here.
    void acknowledged(const std::string& peer, uint64_t streamAcked);

    // A cut (CLEANTXBUFFER): the session's stream from streamFrom on never
    // went. FileData records that began there are gone, so a file still
    // going carries on from the first of them, and a part sent counts only
    // what went before.
    void cut(const std::string& peer, uint64_t streamFrom);
    // Goes up each time a file of ours stops (cancelled here or there)
    // with bytes handed to the host the far end hasn't acknowledged: worth
    // a cut, where the host can make one.
    uint64_t cutsWanted() const { return cutsWanted_; }

    // Of a FileData still coming in from the station, these bytes of the
    // file, from offset, have arrived (see
    // protocol::StreamReader::partialFileData). If the transfer ends
    // before the rest of the record comes, they are kept with the part.
    void arriving(const std::string& peer, uint32_t offer, uint32_t offset, const uint8_t* data, size_t bytes);

    void onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs);

    // The far end's Hello says it takes no files: those waiting for it
    // end NotTaken.
    void notTaken(const std::string& peer);

    // The session with the station has ended: what was under way in it
    // failed, and a file still waiting its turn waits for the next
    // session.
    void sessionEnded(const std::string& peer, uint64_t nowMs);

    // No session could be had with the station: its files fail.
    void noSession(const std::string& peer, const std::string& why);

    // The operator let go of the station: everything with it is cancelled
    // here. inSession: a session with it is up, so the far end is told what
    // became of the files offered in it, by records the session's owner
    // writes before it ends the session; otherwise nothing is sent.
    void release(const std::string& peer, bool inSession);

    // The link stopped: everything still going failed.
    void stopAll(const std::string& why);

    // Expires offers left unanswered, and part files kept too long.
    void tick(uint64_t nowMs);

    // Part files kept for a transfer to carry on in (not parts sent).
    size_t keptCount() const;

    // Sending is not allowed here (receive only): every file going either
    // way fails, the far end of one already offered told so in the
    // session, and nothing new is offered, accepted or sent until it is
    // allowed again. Only records that end a transfer still go.
    void setInhibited(bool inhibited);
    bool inhibited() const { return inhibited_; }

    // Records are waiting to be written to the station.
    bool hasRecords(const std::string& peer) const;

    // The station a file of ours waits for a session with, the one
    // waiting longest, and since when.
    bool wantsSession(std::string& peer, uint64_t& sinceMs) const;

    // Something is under way with the station, or waiting for it.
    bool busyWith(const std::string& peer) const;

private:
    // A FileData of ours in the session's stream.
    struct Piece
    {
        uint64_t recordStart;   // where in the session's stream its record starts
        uint64_t streamStart;   // and its file bytes
        uint64_t fileStart;     // and where in the file
        uint64_t length;
    };

    struct Entry
    {
        FileTransfer transfer;
        uint32_t offer = 0;         // the offer's sequence number in the session; 0 until offered
        uint64_t sinceMs = 0;       // Waiting: since queued; Offered and Asking: since offered
        std::ifstream in;           // ours, once accepted
        std::ofstream out;          // theirs, once accepted
        std::filesystem::path part; // where out writes, while it does
        uint64_t written = 0;       // ours: bytes of the file written into the session
        bool awaitingSaved = false; // ours, cancelled with all of it written: a FileSaved still delivers it
        std::deque<Piece> pieces;   // ours, written and not yet all acknowledged
        uint64_t arriving = 0;      // theirs: file bytes of the record coming in, to the step
        // Theirs: those bytes themselves, from arrivingAt in the file.
        uint64_t arrivingAt = 0;
        std::vector<uint8_t> arrivingData;
        bool spoiled = false;       // theirs: what is written can't be trusted, so it is not kept
    };

    // A part file kept from a transfer of theirs that ended unfinished,
    // or (outgoing) part of ours sent.
    struct Kept
    {
        uint64_t id = 0;
        bool outgoing = false;
        std::string peer;
        std::string name;               // the name offered, made safe
        uint64_t size = 0;
        protocol::Digest digest{};
        std::string path;               // where it was to be saved, or the file sent
        std::filesystem::path part;     // empty for one sent
        uint64_t bytes = 0;             // held, or acknowledged by the far end's modem
        uint64_t handed = 0;            // outgoing: handed to the host
        uint32_t offer = 0;             // outgoing: the offer, this session
        std::deque<Piece> pieces;       // outgoing: still going out, as the host sends what it has
        uint64_t keptAtMs = 0;
        uint64_t keepUntilMs = 0;       // 0: KEPT_MS from keptAtMs
        uint64_t transferId = 0;        // the transfer that left it, this run
        bool hasEnding = false;
        FinishedFile ending;            // see KeptPart
    };

    Entry* find(uint64_t id);
    Entry* findOutgoing(const std::string& peer, uint32_t offer);
    Entry* findIncoming(const std::string& peer, uint32_t offer);
    void queue(const std::string& peer, protocol::Body body);
    void queueCancel(Entry& entry, protocol::CancelReason reason);
    void finish(Entry& entry, FileTransfer::State state, const std::string& error = std::string());
    std::filesystem::path freePart(const std::string& path) const;
    bool openPart(Entry& entry, const std::string& path, std::string& error);
    bool resumePart(Entry& entry, const std::string& path);
    std::list<Kept>::iterator findKept(const Entry& entry);
    std::list<Kept>::iterator findSent(const Entry& entry);
    void keep(Entry& entry);
    void keepSent(Entry& entry);
    // Moves done on by what the far end's modem has acknowledged.
    static bool advance(std::deque<Piece>& pieces, uint64_t streamAcked, uint64_t& done);
    // ended: the file is done for (deleted, too old, too many), so its
    // ending goes to takeEnded(); otherwise another transfer carries on.
    void dropKept(std::list<Kept>::iterator it, bool ended);
    void endSent(std::list<Kept>::iterator it, bool delivered);
    void stopSent(const std::string& peer);
    void limitKept(uint64_t nowMs);
    void receive(Entry& entry, const protocol::FileData& data);
    void completeReceive(Entry& entry);
    // Renames the part file to the transfer's path (or, taken without
    // asking, the next free name beside it); false if it can't be.
    bool placePart(Entry& entry);
    void autoAccept(Entry& entry);
    void prune();
    void changed() { changes_++; }

    std::list<Entry> entries_;
    std::list<Kept> kept_;
    uint64_t nowMs_ = 0;    // the latest time given
    std::vector<FinishedFile> ended_;   // for takeEnded()
    std::map<std::string, std::vector<protocol::Body>> pending_;   // records waiting for each station's session
    uint64_t nextId_ = 1;
    uint64_t changes_ = 0;
    uint64_t cutsWanted_ = 0;
    std::string autoFolder_;
    std::set<std::string> autoCalls_;
    std::string pictureFolder_;
    uint64_t pictureMax_ = 0;
    bool inhibited_ = false;
};

} // namespace files
} // namespace ragtime

#endif
