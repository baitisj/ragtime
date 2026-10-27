// Files broadcast on the D2G group, both ways (docs/PLAN.md, "File
// transfer"; docs/PROTOCOL.md, "Broadcast files").
//
// Ours. A file of at most 300 KB is announced (a FileAnnounce, an item of
// the broadcast numbering) and sent once in pieces (FilePiece records,
// sized to fill whole codewords in its mode), one burst at a time with a
// gap between, so other stations and our own messages get a word in. The
// announce goes again ahead of every ANNOUNCE_EVERY_PIECES pieces. Once
// the last piece has gone it is sealed at once, and Seals answers repair
// requests for it while the seal is open, with parity pieces made from
// the whole file as they are asked for. Nothing acknowledges receipt:
// stations will not confirm.
//
// Theirs. Pieces are kept as they come, before their announce too. A file
// announced is a card with Receive and Ignore, or is received at once
// into the received folder when that is on. Its missing pieces are asked
// for (through Seals) only once it is being received, as parity pieces
// (docs/PROTOCOL.md, "Parity"): as many as it lacks, and more for the
// share it expects to lose; any of them fill any gap, so once the pieces
// and parity pieces held are as many as the file's pieces, the missing
// ones are rebuilt. With every piece in, the whole file's digest is checked against the announce's; a match
// is saved, a mismatch throws the pieces away and asks for them all again
// once, then fails.
//
// No sockets and no clock: HostLink is the socket-free one the app feeds,
// and the time is passed in (Unix ms; records are stamped with it).
#ifndef RAGTIME_BROADCAST_BROADCAST_FILES_H
#define RAGTIME_BROADCAST_BROADCAST_FILES_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/broadcast/Airtime.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

// One file going to or coming from the group, as the screen shows it.
struct BroadcastFile
{
    enum class State
    {
        // Ours.
        Sending,        // the first pass of pieces
        Repairing,      // sealed: taking repair requests (stopped: the first pass cut short)
        Sent,           // repairs closed
        Cancelled,      // stopped before its pieces had all gone, and repairs closed

        // Theirs.
        Heard,          // pieces kept as they come; the operator has not said
        Receiving,      // Receive pressed, or received automatically
        Ignored,
        Saved,
        Incomplete,     // stopped, or repairs ended with pieces missing; what came is kept a while
        Failed,         // the digest didn't match twice, or it couldn't be saved
        Expired,        // heard, never answered, and dropped
    };

    uint64_t id = 0;            // this run's own, for the screen to name it by
    bool outgoing = true;
    std::string from;           // the sender; ours: our callsign
    uint32_t seq = 0;           // the announce's item number
    std::string name;           // empty until announced; a received one made safe to save under
    uint64_t size = 0;          // 0 until announced
    uint16_t pieces = 0;        // 0 until announced
    uint16_t held = 0;          // ours: pieces sent in the first pass; theirs: pieces held
    State state = State::Heard;
    std::string mode;           // ours: the mode it goes in
    uint64_t startedMs = 0;     // ours: sent; theirs: first heard
    uint64_t lastMs = 0;        // a piece last went or came
    bool autoReceived = false;
    bool sealed = false;        // ours: its seal has gone; theirs: its seal was heard
    bool asking = false;        // theirs: a repair request of ours waits to go
    bool complete = false;      // theirs: every piece is here and the digest matched
    // Theirs: its seal was heard, but over MAX_OPEN_MS ago, so the sender's
    // repairs have closed and Ask again has nothing to ask under.
    bool sealAged = false;
    // Theirs: the sender refused to repair it (a protocol::RefusalReason;
    // refusalText says it in words). It is Incomplete, and can't be asked
    // for again.
    std::optional<uint8_t> refusal;
    // Ours: the operator stopped the first pass (held of pieces went);
    // theirs: the operator stopped asking for it.
    bool stopped = false;
    std::string path;           // the file sent, or where it is saved (or is to be); UTF-8
    std::string error;
    uint64_t repairsUntilMs = 0;// ours: when repairs close, as things stand; 0 once closed
    bool keptOpen = false;      // ours: the operator moved repairsUntilMs
    size_t askers = 0;          // ours: stations that have asked for repairs
    // Ours: the stations that asked for its pieces; the repair round going
    // now, whom it answers and its repair pieces of this file gone of
    // those in it (roundPieces 0: none in the round); and repair pieces
    // of it sent since its repairs last opened (Seals::FileRepairs).
    std::vector<std::string> askedBy;
    std::vector<std::string> repairingFor;
    uint32_t roundPieces = 0;
    uint32_t roundSent = 0;
    uint32_t repairPieces = 0;
    // Theirs: repair (parity) pieces held, each standing in for a piece
    // missing: once held and these make pieces, the missing are rebuilt.
    uint16_t repairHeld = 0;
    // Ours: the operator's repair gear for it (empty: Auto), the airtime
    // its repairs have taken, and whether they closed at the cap.
    std::string repairGear;
    double repairSeconds = 0.0;
    bool repairCapped = false;
    // Ours, finished: its seal is still held and the file on disk is the
    // one sent, so its repairs can open again (reopen).
    bool reopenable = false;
    uint64_t expiresAtMs = 0;   // theirs: when a kept or unanswered one is dropped
    // Each piece: files::PIECE_WAITING, PIECE_HELD or PIECE_MISSED, and
    // theirs PIECE_COVERED for as many missing as repairHeld.
    std::vector<uint8_t> pieceMap;
    // An inline picture (protocol::FILE_PICTURE): shown among the messages
    // as it comes.
    bool picture = false;

    bool live() const;
};

// "Sending", "Repairing", "Sent", "Cancelled", "Heard", "Receiving",
// "Ignored", "Saved", "Incomplete", "Failed", "Expired".
const char* stateLabel(BroadcastFile::State state);

// Ours, sealed: its repairs in a few words, for its card and row. A round
// going: "sending repairs to KC2G, 12 of 39 pieces"; else who asked and
// what has gone, "KC2G asked, 39 repair pieces sent", or "no requests yet".
std::string repairText(const BroadcastFile& file);
// "KC2G", "KC2G and W1AW", "KC2G, W1AW and 2 more".
std::string callsText(const std::vector<std::string>& calls);

class BroadcastFiles : public SealedFiles
{
public:
    // Ragtime's cap on anything over HF.
    static constexpr uint64_t MAX_FILE_BYTES = 300 * 1024;
    // The first pass may take this much airtime; longer wants a faster
    // gear. Over WARN_AIR_SECONDS the operator is warned first.
    static constexpr double REFUSE_AIR_SECONDS = 2 * 60 * 60;
    static constexpr double WARN_AIR_SECONDS = 15 * 60;
    // A burst of pieces the host didn't send goes once more.
    static constexpr int BURST_TRIES = 2;
    // Pieces kept of files not being received (heard before their
    // announce, or not yet answered), at most; the oldest go first.
    static constexpr size_t SPOOL_BYTES = 2 * 1024 * 1024;
    // A file being received with nothing new this long after its seal is
    // Incomplete; what came is kept this long for the same file sent
    // again, and a file heard and never answered is dropped after it.
    static constexpr uint64_t INCOMPLETE_MS = MAX_OPEN_MS + 10 * 60 * 1000;
    static constexpr uint64_t KEEP_MS = 24ull * 60 * 60 * 1000;
    // An Incomplete part is kept this long, as a session's kept part is.
    static constexpr uint64_t PART_KEEP_MS = 7ull * 24 * 60 * 60 * 1000;
    // Parts are written to disk at most this often while they change.
    static constexpr uint64_t PART_SAVE_MS = 5 * 1000;
    // Finished transfers kept on the list for the screen.
    static constexpr size_t MAX_FINISHED_KEPT = 50;

    BroadcastFiles(link::HostLink& link, BroadcastChat& chat, Seals& seals);
    ~BroadcastFiles() override;

    BroadcastFiles(const BroadcastFiles&) = delete;
    BroadcastFiles& operator=(const BroadcastFiles&) = delete;

    //---------------------------------------------------------------------
    // The operator's side
    //---------------------------------------------------------------------

    // Broadcasts a file in mode (empty: the group's mode as it stands).
    // 0, with the reason, if it can't go: empty, unreadable, over 300 KB,
    // or longer on the air than REFUSE_AIR_SECONDS.
    // picture: it is an inline picture, a JPEG 2000 codestream.
    uint64_t send(const std::string& path, const std::string& mode, uint64_t nowMs, std::string& error,
                  bool picture = false);
    // What send would take, without sending.
    FileEstimate estimate(uint64_t size, const std::string& name, const std::string& mode) const;

    // Ours: stops the first pass. What has gone is sealed and repairs stay
    // open as for any share, so a station that asks for the rest on the
    // group gets it, all of it if need be, while the seal is fresh; with
    // nothing gone yet it simply ends. Once repairing: closes repairs.
    bool cancel(uint64_t id);
    // Ours, sealed: repairs stay open until then, or close now.
    bool keepOpen(uint64_t id, uint64_t untilMs);
    bool closeRepairs(uint64_t id);
    // Ours, finished (Sent, or stopped part way) while its seal is still
    // held (MAX_OPEN_MS): repairs open again until then. The file is read
    // again from where it was sent, and must be the same bytes. 0, with the
    // reason, if it can't.
    bool reopen(uint64_t id, uint64_t untilMs, uint64_t nowMs, std::string& error);
    // A finished broadcast of ours (the Files tab's Completed row) by the
    // file it sent: its id, or 0.
    uint64_t findSent(const std::string& path, uint64_t size) const;
    // Ours: the gear its repairs go in (empty: Auto, from the askers' S/N).
    bool setRepairGear(uint64_t id, const std::string& mode);

    // Theirs: save it at path once all of it is here (at once if it is),
    // asking for what is missing until then; an empty path keeps the one
    // given before (Ask again on an Incomplete one). Stop: no more asking,
    // what came is kept as Incomplete. Ignore (Reject file): throw away
    // what came, logged as Declined.
    bool receive(uint64_t id, const std::string& path, uint64_t nowMs, std::string& error);
    bool stop(uint64_t id, uint64_t nowMs);
    bool ignore(uint64_t id);
    // Ours or theirs, finished: off the list.
    bool remove(uint64_t id);

    // Theirs not yet whole (Heard, Receiving, Incomplete) are kept in this
    // folder (BroadcastParts), a file each, and read back from it now: so
    // a restart loses neither the pieces nor the seals to ask under. They
    // come back on the Files tab, not as cards in the chat.
    void setPartsFolder(const std::string& folder, uint64_t nowMs);
    // Writes what has changed since (the destructor does it too).
    void saveParts(uint64_t nowMs);

    // Files announced from now on are received without asking into this
    // folder (under the name announced, numbered when one is there
    // already); empty: they wait for Receive.
    void setAutoReceive(const std::string& folder);
    const std::string& autoReceive() const { return autoFolder_; }
    // Pictures of at most maxBytes are received without asking into this
    // folder, so they show as they come; empty: they wait as other files do.
    void setAutoPictures(const std::string& folder, uint64_t maxBytes);

    // A picture's bytes as they stand: all of one of ours or one saved, or
    // the pieces held of one coming, from the first up to the first one
    // missing (any first part of a picture shows). False if there are none.
    bool pictureBytes(uint64_t id, std::vector<uint8_t>& out) const;

    std::vector<BroadcastFile> files(uint64_t nowMs) const;
    bool find(uint64_t id, uint64_t nowMs, BroadcastFile& out) const;
    // The Files tab's rows for those still going (package G's FileRow).
    std::vector<files::FileRow> rows(uint64_t nowMs) const;
    // Transfers that have ended since last asked, for the Files tab's log.
    std::vector<files::FinishedFile> takeFinished();

    struct Event
    {
        enum class Type
        {
            Announced,  // a file of theirs is first known: a card for it
            Sending,    // a file of ours has started: a card for it
            Saved,      // a file of theirs is saved
            Ended,      // ours or theirs is over (Sent, Failed, Ignored, ...)
            Repairing,  // a repair round of ours has started to carry pieces of it
            Reopened,   // ours: its repairs opened again; theirs: its sender's did, and we ask again
        } type = Type::Announced;
        uint64_t id = 0;
    };
    std::vector<Event> takeEvents();

    // Moves with every change the screen shows.
    uint64_t version() const { return version_ + seals_.version(); }

    //---------------------------------------------------------------------
    // The host's side
    //---------------------------------------------------------------------

    void onEvent(const link::HostLink::Event& event, uint64_t nowMs);
    // Every record heard on the group (BroadcastChat::onEvent gives them).
    void onRecord(const protocol::Record& record, uint64_t nowMs);
    // Moves the first pass on, one burst at a time, and the timers.
    void tick(uint64_t nowMs);

    // SealedFiles.
    uint16_t pieceCount(uint32_t seq) const override;
    bool streaming(uint32_t seq) const override;
    bool piece(uint32_t seq, uint16_t index, uint32_t time, protocol::Record& out) override;
    bool stopped(uint32_t seq) const override;
    bool wanted(const std::string& call, uint32_t seq, uint32_t& need, uint32_t& want) const override;
    void asked(const std::string& call, uint32_t seq, uint32_t want) override;

private:
    struct Outgoing
    {
        BroadcastFile file;
        protocol::Record announce;
        std::vector<uint8_t> content;   // freed once it has ended
        std::vector<std::vector<uint8_t>> slices;   // its pieces, made for parity when first asked for
        uint16_t pieceBytes = 0;
        uint16_t next = 0;              // the next piece of the first pass
        int lastBlock = -1;             // the last block of pieces an announce went ahead of
        uint64_t burst = 0;             // the first pass's burst with the host
        std::vector<std::vector<uint8_t>> frames;   // its frames, to send again
        std::vector<uint16_t> burstPieces;          // the pieces in it
        int tries = 0;
        bool firstBurst = false;        // it carries the announce for the first time
        uint64_t nextMs = 0;            // the next burst goes no sooner
        files::RateMeter rate;
        uint64_t firstPassMs = 0;       // when the first pass ended, for the bps column
        bool finished = false;          // its FinishedFile is out
        bool capped = false;            // its repairs closed at the airtime cap
        bool inRound = false;           // a repair round carrying it was told of (Event Repairing)
    };

    struct Incoming
    {
        BroadcastFile file;
        bool announced = false;
        protocol::FileAnnounce announce;
        std::map<uint16_t, std::vector<uint8_t>> pieces;
        size_t bytes = 0;               // in pieces
        // Parity pieces (index at or past the piece count) until there are
        // enough to rebuild the pieces missing.
        std::map<uint16_t, std::vector<uint8_t>> parity;
        size_t parityBytes = 0;
        // The share of what is sent that we lose: of the last round (pieces
        // that came of those asked for), else unknown (below 0).
        double loss = -1.0;
        uint32_t askedWant = 0;         // parity pieces last asked for, by us or a request speaking for ours
        uint32_t came = 0;              // pieces and parity pieces heard since
        // Parity pieces by index: a sender numbers them one after another,
        // so from the first after those heard when we first asked
        // (parityFrom) to the highest heard (parityTop) is how many went,
        // though a round cut short sent fewer than were asked for; and
        // parityHeard of them came.
        uint16_t parityFrom = 0;        // 0: not asked yet
        uint16_t parityTop = 0;         // 0: none heard
        uint32_t parityHeard = 0;
        int digestFailures = 0;
        bool chosenPath = false;        // the operator gave the path
        files::RateMeter rate;
        bool finished = false;
        bool sealSeen = false;          // a seal listing it was heard
        bool refusedOut = false;        // Incomplete as the sender refused: back to Receiving if it re-opens
        protocol::Record announceRecord;
        std::string partFile;           // its file in the parts folder, once written
        std::string savedSig;           // what was last written, in short
    };

    Outgoing* findOutgoing(uint64_t id);
    Outgoing* outgoingBySeq(uint32_t seq);
    const Outgoing* outgoingBySeq(uint32_t seq) const;
    Incoming* findIncoming(uint64_t id);
    Incoming* incomingFor(const std::string& call, uint32_t seq);
    void nextBurst(Outgoing& out, uint64_t nowMs);
    void firstPassDone(Outgoing& out, uint64_t nowMs);
    void finishOutgoing(Outgoing& out, BroadcastFile::State state, uint64_t nowMs);
    void heardAnnounce(const protocol::Record& record, uint64_t nowMs);
    void heardPiece(const protocol::Record& record, uint64_t nowMs);
    void tryComplete(Incoming& in, uint64_t nowMs);
    void tryRebuild(Incoming& in);
    double lossOf(const Incoming& in) const;
    void clearPieces(Incoming& in);
    bool save(Incoming& in, std::string& error);
    void finishIncoming(Incoming& in, BroadcastFile::State state, uint64_t nowMs, const std::string& error = {});
    void dropPart(Incoming& in);
    void loadParts(uint64_t nowMs);
    void writeParts();
    void limitSpool();
    BroadcastFile view(const Outgoing& out, uint64_t nowMs) const;
    BroadcastFile view(const Incoming& in, uint64_t nowMs) const;
    void prune();
    void emit(Event::Type type, uint64_t id) { events_.push_back({type, id}); }
    void changed() { version_++; }

    link::HostLink& link_;
    BroadcastChat& chat_;
    Seals& seals_;
    std::list<Outgoing> outgoing_;
    std::list<Incoming> incoming_;
    std::string partsFolder_;
    uint64_t partsSavedMs_ = 0;
    std::vector<files::FinishedFile> finished_;
    std::vector<Event> events_;
    std::string autoFolder_;
    std::string pictureFolder_;
    uint64_t pictureMax_ = 0;
    uint64_t nextId_ = 1;
    uint64_t version_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
