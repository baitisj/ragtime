// Seals and repair on the D2G group (docs/PLAN.md, "Seals, receipts and
// repairs"; docs/PROTOCOL.md, "Seals and repair"), the way NACK-oriented
// multicast (NORM) repairs: listeners say only what they lack, after a
// random wait, and keep quiet when someone else has asked for it already.
//
// Ours. Every file we broadcast (its FileAnnounce) is listed in a Seal once
// its burst has gone, and so are our Texts when the operator asks for that
// (setSealText, off by default: on a poor link a seal can take 30 to 60 s
// of airtime, and plain chat must never pay for it unasked): the seal names our first item since
// the last seal and gives each one's record digest, zeros for a number
// whose burst never went. Each seal names the one before by its digest,
// so a listener that missed one sees the gap. A seal goes once an over
// has ended (QUIET_MS with nothing new of ours), as soon as a file's
// pieces have all gone, and at the latest SEAL_EVERY_MS after the oldest
// item it lists.
//
// After a seal goes, a listening window (3 one-codeword bursts in its
// mode, plus 4 s). The requests heard in it are joined and answered with
// parity (docs/PROTOCOL.md, "Parity"): each request says what its asker
// lacks and how many repair symbols it wants (more than it lacks, for what
// it expects to lose), and one parity symbol fills a different gap for
// every asker, so the round sends what the neediest asker wants, not the
// union of every gap. A file gets parity pieces; chat lines, which differ
// in length, get the lines themselves when they are cheaper, else
// ItemParity over the lines asked for. The seals it falls under go at the
// end, which opens the next window. Each request says the S/N its asker
// heard us at, so each item goes again in the fastest gear every station
// that asked for it can copy (Auto's sum, with its margin), never faster
// than it first went: gear by gear, fastest first, the seals last in the
// slowest gear used. Slower gears than the round's own may take at most
// setSlowResendSeconds of airtime a round; the rest waits for the next.
// What we won't repair (a seal closed, a file cancelled or no longer
// held) is refused once a window, in a RepairRefused, so listeners stop
// asking. A round with no requests ends it. A
// request heard with no window open (a late joiner, or an operator who
// pressed Receive later) opens one. Requests are answered while the seal
// is open: it is one of our NEWEST_OPEN newest seals, or younger than
// OPEN_AIRTIMES times the airtime its items took, and never older than
// MAX_OPEN_MS, unless the operator keeps it open longer or closes it now.
//
// Theirs. The items and seals heard from every station are kept for
// MAX_OPEN_MS. When a station's seal comes, what we lack under it (and
// under its other recent seals, and a missing previous seal) is asked for
// in one RepairRequest a seal, at a random time within the window, with
// the mean S/N of the station's last few bursts, and a want for
// each lack sized by the share we lost of the last round (or of the first
// pass). A request heard from another station first speaks for ours when
// its asker's gear is no faster than ours and it wants at least as much
// (and, for chat lines, asks for every line we lack); ours goes only for
// what is left. ItemParity heard rebuilds the lines it covers once enough
// of it is here (takeRebuilt). A refusal heard stops asking for what it
// names. A file's pieces are asked for only while the operator receives
// it (BroadcastFiles says which). A file heard with no seal over it
// SEAL_LATE_MS after the last of it came lost every copy of its seal: a
// request under seal 0 names the file, and the sender sends the seal over
// it again (or refuses), so the asking can start. Likewise a listener
// that still lacks something under a seal, with nothing heard from its
// sender and no request since for SEAL_LATE_MS, lost the seal that ended
// the last round: it asks again without it.
//
// No sockets and no clock: HostLink is the socket-free one the app feeds,
// the time is passed in, and the randomness is seeded.
#ifndef RAGTIME_BROADCAST_SEALS_H
#define RAGTIME_BROADCAST_SEALS_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastParts.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

// What Seals needs from the broadcast files: the pieces of our own files,
// and which pieces of other stations' files the operator wants.
class SealedFiles
{
public:
    virtual ~SealedFiles() = default;

    // Ours: the file announced as item seq has this many pieces (0: not a
    // file of ours, or no longer held).
    virtual uint16_t pieceCount(uint32_t seq) const = 0;
    // Ours: its first pass is still going, so its announce isn't sealed yet.
    virtual bool streaming(uint32_t seq) const = 0;
    // Ours: piece index of it as a FilePiece record, stamped with time; an
    // index at or past pieceCount is a parity piece.
    virtual bool piece(uint32_t seq, uint16_t index, uint32_t time, protocol::Record& out) = 0;
    // Ours: the operator cancelled it (stopped its first pass).
    virtual bool stopped(uint32_t seq) const
    {
        (void)seq;
        return false;
    }
    // Theirs: of call's file (announce seq), how many pieces we lack (need)
    // and how many parity pieces to ask for (want); false when we want
    // none of it (not receiving it, or it is whole).
    virtual bool wanted(const std::string& call, uint32_t seq, uint32_t& need, uint32_t& want) const = 0;
    // Theirs: a request for want parity pieces of it has gone, ours or one
    // that spoke for ours, so the next round shows what share we lose.
    virtual void asked(const std::string& call, uint32_t seq, uint32_t want)
    {
        (void)call;
        (void)seq;
        (void)want;
    }
};

// A refusal's reason in words, after the sender's call: "stopped sharing
// it", "cancelled it", "no longer holds it", "closed its repairs"; "won't
// repair it" for a reason this version doesn't know.
const char* refusalText(uint8_t reason);

class Seals
{
public:
    // When a seal goes.
    static constexpr uint64_t QUIET_MS = 60 * 1000;
    static constexpr uint64_t SEAL_EVERY_MS = 10 * 60 * 1000;
    // A seal burst the host doesn't send goes again, this many times in all.
    static constexpr int SEAL_TRIES = 3;
    // A seal over a file goes again after a window that brought no
    // requests, this many more times, in case it or a request was lost
    // (NORM's flush, repeated).
    static constexpr int FLUSH_REPEATS = 2;
    // The sensitivity window.
    static constexpr size_t NEWEST_OPEN = 3;
    static constexpr double OPEN_AIRTIMES = 4.0;
    // One resend round carries at most this much airtime of pieces; the
    // rest is asked for again next round.
    static constexpr double RESEND_MAX_SECONDS = 10 * 60;
    // A listener stops asking under a seal after this many requests that
    // brought it nothing.
    static constexpr int MAX_ASKS = 5;
    // A late request (Receive pressed after the seal) waits up to this long.
    static constexpr uint64_t LATE_SPREAD_MS = 10 * 1000;
    // The S/N a request carries: the mean of the sender's last SNR_BURSTS
    // heard in SNR_WINDOW_MS, the figure the gearshift's Auto shifts for.
    // Not the lowest (Jeff, 2026-10-25): one deep fade put every repair in
    // the slowest gear while the path ran 10 dB above it, and the parity
    // covers the bursts a fade takes.
    static constexpr size_t SNR_BURSTS = gearbox::PathSnr::BURSTS;
    static constexpr uint64_t SNR_WINDOW_MS = 15 * 60 * 1000;
    // Repairs go in the fastest gear needing no more than the asker's S/N
    // less this, as Auto picks.
    static constexpr double REPAIR_MARGIN_DB = gearbox::AUTO_MARGIN_DB;
    // Airtime a round may spend in gears slower than its own, by default
    // (Settings, "Slow repairs").
    static constexpr double SLOW_RESEND_SECONDS = 5 * 60;
    // The same refusal goes at most once in this long.
    static constexpr uint64_t REFUSE_EVERY_MS = 10 * 60 * 1000;
    // A file heard with no seal over it this long after the last of it
    // came: every copy of its seal was lost, so ask for it by the file.
    static constexpr uint64_t SEAL_LATE_MS = 3 * 60 * 1000;
    // Asking for it goes on longer than under a seal heard: a request and
    // a short seal are cheap, and each copy of the seal may be lost too.
    static constexpr int MAX_SEAL_OVER_ASKS = 10;

    Seals(link::HostLink& link, BroadcastChat& chat);

    void setFiles(SealedFiles* files) { files_ = files; }
    void setRandomSeed(uint32_t seed) { random_.seed(seed); }
    // The mode our requests and seals go in when nothing else says (the
    // gear's mode, or empty for the group's).
    void setMode(const std::string& mode) { mode_ = mode; }
    // The gearshift's ladder, most robust first: the gears repairs may go
    // in. Empty, repairs go in the gear the round would use anyway.
    void setLadder(std::vector<gearbox::Gear> ladder) { ladder_ = std::move(ladder); }
    void setSlowResendSeconds(double seconds) { slowResendSeconds_ = seconds; }
    double slowResendSeconds() const { return slowResendSeconds_; }
    // Andrew's three (2026-10-25, Jeff took all three). The floor: repairs
    // never go slower than this mode, though an asker says it hears us
    // worse (empty: no floor). A file's own gear (the operator's, from its
    // menu): its pieces go again in that mode whatever the askers say, and
    // the round's queued repairs of it are taken back so the next round
    // goes in it (empty: Auto). The cap: a file's repairs close once their
    // airtime reaches it (0: no cap).
    void setRepairFloor(const std::string& mode) { floor_ = mode; }
    const std::string& repairFloor() const { return floor_; }
    void setRepairGear(uint32_t seq, const std::string& mode);
    std::string repairGear(uint32_t seq) const;
    void setRepairCapSeconds(double seconds) { repairCapSeconds_ = seconds; }
    double repairCapSeconds() const { return repairCapSeconds_; }
    // The airtime a file's repairs have taken (bursts sent), and whether
    // that has reached the cap.
    double repairAirtime(uint32_t seq) const;
    bool repairCapReached(uint32_t seq) const;
    // Our Texts are sealed too (Settings, "Seal my broadcast messages").
    // Off, a Text is never listed, so a chat over goes with no seal and no
    // repair; a file is sealed either way. It applies to Texts sent from
    // now on.
    void setSealText(bool on) { sealText_ = on; }
    bool sealText() const { return sealText_; }

    // Our seal chain, kept across restarts with the broadcast numbering:
    // the newest seal's number and digest. A new callsign starts afresh.
    struct Chain
    {
        uint16_t number = 0;
        protocol::Digest digest{};
    };
    Chain chain() const { return {lastNumber_, lastDigest_}; }
    void setChain(const std::string& callsign, const Chain& chain);

    //---------------------------------------------------------------------
    // Ours
    //---------------------------------------------------------------------

    // Records of ours handed to HostLink as burst, in mode: the items among
    // them are sealed once it has gone. An item sent again (Send again) is
    // taken back into its place.
    void going(const std::vector<protocol::Record>& records, uint64_t burst, const std::string& mode,
               uint64_t nowMs);
    // More airtime an item took: a file's pieces, counted to its announce.
    void addAirtime(uint32_t seq, double seconds);
    // A file's pieces have all gone: seal now, not after QUIET_MS.
    void sealSoon() { sealSoon_ = true; }
    // When our next seal is due, for the status bar's countdown (Jeff,
    // 2026-10-25): items of ours have gone with no seal yet, and none is
    // still going. It may be now or past while a resend round, a seal
    // burst or Pause sending holds it. Nothing while no seal waits.
    std::optional<uint64_t> nextSealMs() const;

    // A seal of ours as the screen shows it.
    struct SealView
    {
        bool sealed = false;        // listed in a seal that has gone
        uint16_t number = 0;
        bool open = false;          // taking repair requests
        uint64_t openUntilMs = 0;   // when it closes, as things stand; 0 once closed
        bool resending = false;     // a round of repairs under it is going
        size_t askers = 0;          // stations that have asked under it
        size_t asks = 0;            // their requests
        bool keptOpen = false;      // the operator set openUntilMs
    };
    // The seal our item seq is listed in.
    SealView sealOf(uint32_t seq, uint64_t nowMs) const;
    // A file of ours, as its card and Files tab row show its repairs
    // (Jeff, 2026-10-26: a round of 39 repair pieces went out unseen).
    struct FileRepairs
    {
        // Stations that asked for its pieces under its seal, in the order
        // they first asked.
        std::vector<std::string> askers;
        // The round going now: the stations it answers, its repair pieces
        // of this file still to go or gone (0: none of it in the round).
        std::vector<std::string> sendingTo;
        uint32_t roundPieces = 0;
        uint32_t roundSent = 0;
        // Repair pieces of it sent since its repairs last opened.
        uint32_t piecesSent = 0;
    };
    FileRepairs fileRepairs(uint32_t seq) const;
    // The operator keeps it open until then (re-opening it, if closed), or
    // closes it now: the round's repairs under it still with HostLink are
    // taken back (the burst on the air goes on).
    bool keepOpen(uint32_t seq, uint64_t untilMs);
    bool closeNow(uint32_t seq);
    // Closed (by the operator or the cap) or out of its window, while still
    // held: open until then, its files' repair airtime counted afresh, and
    // the seal sent again so stations we refused ask once more.
    bool reopen(uint32_t seq, uint64_t untilMs, uint64_t nowMs);

    //---------------------------------------------------------------------
    // Everything heard and settled
    //---------------------------------------------------------------------

    void onEvent(const link::HostLink::Event& event, uint64_t nowMs);
    // Every record heard on the group (BroadcastChat::onEvent gives them).
    void onRecord(const protocol::Record& record, uint64_t nowMs);
    void tick(uint64_t nowMs);

    //---------------------------------------------------------------------
    // Theirs
    //---------------------------------------------------------------------

    // The operator now wants something under call's seals (pressed
    // Receive): ask soon, without waiting for the next seal.
    void askSoon(const std::string& call, uint64_t nowMs);
    // The operator asks again for what we lack of call's item seq (Ask
    // again): asking under the seal that lists it starts afresh, though
    // earlier requests brought nothing. False when no seal heard in the
    // last MAX_OPEN_MS lists it, so there is nothing to ask under.
    bool askAgain(const std::string& call, uint32_t seq, uint64_t nowMs);
    // We gave up asking under the seal that lists call's item seq: MAX_ASKS
    // requests in a row brought nothing.
    bool gaveUp(const std::string& call, uint32_t seq) const;
    // A seal heard from call lists its item seq.
    bool sealHeard(const std::string& call, uint32_t seq) const;
    // A request of ours waits to go to call.
    bool asking(const std::string& call) const;
    // Items rebuilt from ItemParity since last asked: each is to go through
    // everything a record heard goes through (BroadcastChat::onRebuilt,
    // this onRecord, BroadcastFiles::onRecord, the station list).
    std::vector<protocol::Record> takeRebuilt();
    // The mean S/N of call's last SNR_BURSTS heard in SNR_WINDOW_MS (the
    // host's reports), what our requests to it carry.
    std::optional<double> snrOf(const std::string& call, uint64_t nowMs) const;
    // call refused to repair its item seq: why (a protocol::RefusalReason).
    std::optional<uint8_t> refusal(const std::string& call, uint32_t seq) const;
    // Kept across a restart with a part of call's file (BroadcastParts):
    // the seals heard over its announce seq, and back again with the
    // announce itself (held as of now) and its last piece at lastMs. A
    // request goes soon if a seal is fresh and something is lacked.
    std::vector<KeptSeal> sealsOver(const std::string& call, uint32_t seq) const;
    void restoreHeard(const protocol::Record& announce, const std::vector<KeptSeal>& seals, uint64_t lastMs,
                      uint64_t nowMs);

    //---------------------------------------------------------------------
    // The seal badge: what the screen shows beside each message
    //---------------------------------------------------------------------

    // Silence is not a receipt: with no request heard, a seal says only
    // that it went. A station holds an item of ours when a repair request
    // of its under the seal (it heard the seal) didn't ask for that item.
    enum class Mark
    {
        None,       // not sealed: unsealed chat, or nothing known any more
        Awaiting,   // ours: sealed chat, its seal not gone yet
        Sealed,     // ours: its seal went, nobody has asked for it; theirs: a seal lists it, the digest matches
        Repairing,  // ours: a station asked for it, and it goes again
        Held,       // ours: a station's request shows it holds it (holders)
        Mismatch,   // theirs: the seal's digest differs from the text we hold
    };
    struct ItemMark
    {
        Mark mark = Mark::None;
        uint64_t sealedMs = 0;              // ours: when its seal went; theirs: when it was heard
        std::vector<std::string> holders;   // ours: stations shown to hold it
        std::vector<std::string> wanting;   // ours: stations that asked for it, not yet sent again
        std::vector<std::string> sentAgainFor;  // ours: it went again for these, who haven't asked since
    };
    ItemMark ourMark(uint32_t seq) const;
    ItemMark theirMark(const std::string& call, uint32_t seq) const;

    // Counts, for tests and the status line.
    size_t requestsSent() const { return requestsSent_; }
    size_t requestsHeld() const { return requestsHeld_; }   // dropped: others asked first
    size_t requestsHeard() const { return requestsHeard_; } // to us
    size_t rounds() const { return rounds_; }               // resend rounds of ours
    size_t sealsSent() const { return sealsSent_; }
    size_t refusalsSent() const { return refusalsSent_; }
    size_t refusalsHeard() const { return refusalsHeard_; }
    size_t paritySent() const { return paritySent_; }     // ItemParity and parity pieces of ours
    size_t itemsRebuilt() const { return itemsRebuilt_; }
    // Moves with every change the screen shows.
    uint64_t version() const { return version_; }

private:
    //----- ours

    enum class Fate
    {
        Going,      // with HostLink
        Sent,
        Unsent,     // the host dropped it, or it was taken back
    };

    struct OurItem
    {
        uint32_t seq = 0;
        protocol::Record record;
        protocol::Digest digest{};
        uint64_t burst = 0;
        Fate fate = Fate::Going;
        uint64_t sentMs = 0;
        double air = 0.0;
        std::string mode;
        bool file = false;
        uint32_t nextParity = 0;    // a file's next parity piece, counted from its piece count
    };

    struct OurSeal
    {
        uint16_t number = 0;
        protocol::Record record;
        protocol::Digest digest{};
        uint32_t first = 0;
        size_t count = 0;
        std::string mode;
        double air = 0.0;
        uint64_t burst = 0;
        int tries = 0;
        bool sent = false;
        uint64_t sentMs = 0;
        uint64_t keepUntilMs = 0;   // the operator's: open until then
        bool closed = false;        // the operator closed it
        bool capped = false;        // closed as a file under it reached the repair airtime cap
        std::map<std::string, uint64_t> askers;
        size_t asks = 0;
        // Per file under it, the stations that asked for its pieces, in
        // the order they first asked.
        std::map<uint32_t, std::vector<std::string>> fileAskers;
        int flushes = 0;            // sent again after a quiet window
        uint32_t nextParity = 0;    // the next ItemParity's index
        // Per item, from the requests heard under it: who holds it, and
        // who asked for it and hasn't had it sent again yet.
        std::map<uint32_t, std::set<std::string>> holders, wanting, servedTo;
    };

    // What the requests in a window asked for under one seal: each item in
    // the gear (mode) it goes again in; each asker's items and the repair
    // symbols it wants for them; and for each file the most parity pieces
    // an asker wants in each gear.
    struct Asked
    {
        bool seal = false;
        std::string sealMode;
        std::map<uint32_t, std::string> items;
        std::vector<std::pair<std::set<uint32_t>, uint32_t>> itemAskers;
        std::map<uint32_t, std::map<std::string, uint32_t>> files;
        std::map<uint32_t, std::set<std::string>> fileAskers;
    };

    // A refusal to go at the window's end: for these files under a seal,
    // or all of it.
    struct Refusing
    {
        bool all = false;
        std::set<uint32_t> files;
        std::string mode;
    };

    bool open(const OurSeal& seal, uint64_t nowMs) const;
    uint64_t openUntil(const OurSeal& seal, uint64_t nowMs) const;
    bool newest(const OurSeal& seal) const;
    OurSeal* findSeal(uint16_t number);
    const OurSeal* sealFor(uint32_t seq) const;
    const OurItem* findItem(uint32_t seq) const;
    void sealIfDue(uint64_t nowMs);
    void onRequest(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs);
    void onSealOver(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs);
    bool ours(uint16_t number) const;
    void refuse(uint16_t seal, protocol::RefusalReason reason, uint32_t file, const std::string& mode,
                uint64_t nowMs);
    void openWindow(const std::string& mode, uint64_t nowMs);
    void resend(uint64_t nowMs);
    void prune(uint64_t nowMs);
    link::ModeInfo modeInfo(const std::string& mode) const;
    // Gears by speed: a mode's payload bit/s (0 when unknown), the slower
    // of two (the known one when only one is), and the gear for an S/N.
    double speedOf(const std::string& mode) const;
    std::string slower(const std::string& a, const std::string& b) const;
    std::optional<std::string> gearFor(std::optional<double> snr) const;
    // An item that first went in mode first goes again in this mode for a
    // station that heard us at snr.
    // seq, when given, is the item: its own gear, when the operator set
    // one, wins.
    std::string repairMode(std::optional<double> snr, const std::string& first, uint32_t seq = 0) const;
    std::string faster(const std::string& a, const std::string& b) const;
    // The round's queued bursts that carry nothing but repairs under seal
    // number go back from HostLink.
    void takeBackRepairs(uint16_t number);

    //----- theirs

    struct HeardItem
    {
        protocol::Digest digest{};
        uint8_t type = 0;
        uint64_t heardMs = 0;
        std::vector<uint8_t> bytes;     // its encoded record, a symbol for ItemParity
    };

    struct HeardSeal
    {
        protocol::Seal seal;
        protocol::Digest digest{};
        uint64_t heardMs = 0;
        int asks = 0;               // requests of ours that brought nothing
        size_t lackWhenAsked = 0;
        bool givenUp = false;
        // The share of its items we lose: of the first pass, then of the
        // last round (repair symbols that came of those asked for).
        double itemLoss = -1.0;
        uint32_t itemsAsked = 0;    // repair symbols asked for last, by us or a request speaking for ours
        uint32_t came = 0;          // its items and ItemParity heard since
        uint64_t askedMs = 0;       // we last asked under it, or a request spoke for ours
    };

    // What one request asks for, or what we lack, under one seal: the
    // items, and for each file the pieces lacked (need); with the repair
    // symbols wanted for them.
    struct FileLack
    {
        uint32_t need = 0;
        uint32_t want = 0;
    };
    struct Lack
    {
        uint16_t seal = 0;
        protocol::Digest sealDigest{};  // zeros: the seal itself wasn't heard
        bool wantSeal = false;
        std::vector<bool> items;
        uint32_t itemsWant = 0;
        std::map<uint32_t, FileLack> files;

        bool any() const;
        size_t size() const;
        size_t missingItems() const;
    };

    // Parity over a station's items heard: the seal, the items covered and
    // the symbol length name a batch; its symbols by index.
    struct ParityBatch
    {
        std::map<uint16_t, std::vector<uint8_t>> symbols;
        uint64_t heardMs = 0;
    };
    using ParityKey = std::tuple<uint16_t, std::vector<bool>, size_t>;

    struct Refused
    {
        uint8_t reason = 0;
        bool all = false;
        std::set<uint32_t> files;
        uint64_t heardMs = 0;
    };

    struct Station
    {
        std::map<uint32_t, HeardItem> items;
        std::deque<std::pair<uint64_t, double>> snrs;   // the host's reports, when
        std::map<uint16_t, Refused> refused;            // by seal
        std::map<uint16_t, HeardSeal> seals;
        uint64_t askAtMs = 0;                   // our request goes then; 0: none waiting
        std::map<uint16_t, std::vector<Lack>> covered;  // others' requests heard since it was set
        std::map<uint16_t, int> sealAsks;       // requests for a seal never heard
        // Files we heard pieces of (their announce's number, when): the
        // announce itself may be what was lost.
        std::map<uint32_t, uint64_t> piecesOf;
        std::map<ParityKey, ParityBatch> parity;
        // Requests for the seal over a file (by its announce's number)
        // never heard: how many, and when the last went.
        std::map<uint32_t, std::pair<int, uint64_t>> sealOverAsks;
        uint64_t lastHeardMs = 0;   // anything of its came then
    };

    std::vector<Lack> lacks(const std::string& call, const Station& station, uint64_t nowMs) const;
    void armAsk(const std::string& call, Station& station, uint64_t spreadMs, uint64_t nowMs);
    void ask(const std::string& call, Station& station, uint64_t nowMs);
    void heardSeal(const std::string& call, const protocol::Record& record, uint64_t nowMs);
    void heardRequest(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs);
    void heardRefusal(const std::string& call, const protocol::RepairRefused& refused, uint64_t nowMs);
    void heardSnr(const std::string& call, double dB, uint64_t nowMs);
    void heardItemParity(const std::string& call, const protocol::Record& record, uint64_t nowMs);
    void rebuildItems(const std::string& call, Station& station, const ParityKey& key);
    // Our answer to the items asked for in one gear under seal s: the lines
    // themselves, parity over them, or the lines and parity after them.
    std::vector<protocol::Record> itemRepairs(OurSeal& s, const std::vector<uint32_t>& items, uint32_t want,
                                              const std::string& mode, uint32_t time);
    void forget(uint64_t nowMs);
    void flush(uint64_t nowMs);

    link::HostLink& link_;
    BroadcastChat& chat_;
    SealedFiles* files_ = nullptr;
    std::mt19937 random_;
    std::string mode_;
    std::vector<gearbox::Gear> ladder_;
    double slowResendSeconds_ = SLOW_RESEND_SECONDS;
    std::string floor_;
    std::map<uint32_t, std::string> gears_;     // the operator's, by item
    double repairCapSeconds_ = 0.0;
    std::map<uint32_t, double> repairAir_;      // a file's repairs sent, by its announce
    bool sealText_ = false;
    std::string callsign_;

    std::deque<OurItem> unsealed_;          // in the order they went
    std::map<uint32_t, OurItem> sealedItems_;
    std::deque<OurSeal> seals_;
    uint16_t lastNumber_ = 0;
    protocol::Digest lastDigest_{};
    uint64_t lastItemMs_ = 0;               // our newest item went then
    bool sealSoon_ = false;

    uint64_t windowEndsMs_ = 0;             // 0: no window open
    std::map<uint16_t, Asked> asked_;
    std::map<std::pair<uint16_t, uint8_t>, Refusing> refusing_;   // by seal and reason
    std::map<std::pair<uint16_t, uint8_t>, uint64_t> refusedMs_;  // the last went then
    std::set<uint64_t> resendBursts_;
    uint64_t resendLast_ = 0;               // the round's last burst, with the seals
    // The round's bursts before its seals: the seals they carry repairs
    // under, and each file's share of their airtime.
    struct RoundBurst
    {
        std::set<uint16_t> seals;
        std::map<uint32_t, double> fileAir;
        std::map<uint32_t, uint32_t> filePieces;
    };
    std::map<uint64_t, RoundBurst> roundBursts_;
    // Each file's part in the round going now (FileRepairs), and its
    // repair pieces sent since its repairs last opened.
    struct FileRound
    {
        std::set<std::string> to;
        uint32_t pieces = 0;
        uint32_t sent = 0;
    };
    std::map<uint32_t, FileRound> fileRound_;
    std::map<uint32_t, uint32_t> repairPieces_;
    std::string lastRoundMode_;             // the gear the round's seals went in

    std::map<std::string, Station> stations_;

    // Marks outliving what they came from: our seals once pruned, their
    // items once forgotten. At most MAX_MARKS of each, oldest dropped.
    void settle(const OurSeal& seal);
    void markTheirs(const std::string& call, uint32_t seq, const protocol::Digest& held,
                    const protocol::Digest& sealed, uint64_t nowMs);
    std::map<uint32_t, ItemMark> settled_;
    std::map<std::pair<std::string, uint32_t>, ItemMark> theirMarks_;
    std::deque<std::pair<std::string, uint32_t>> theirOrder_;
    std::set<uint32_t> roundItems_;         // ours, in the repair round going now
    uint64_t forgotMs_ = 0;
    uint64_t lateCheckMs_ = 0;              // files with no seal were last looked for then

    size_t requestsSent_ = 0;
    size_t requestsHeld_ = 0;
    size_t requestsHeard_ = 0;
    size_t rounds_ = 0;
    size_t sealsSent_ = 0;
    size_t refusalsSent_ = 0;
    size_t refusalsHeard_ = 0;
    size_t paritySent_ = 0;
    size_t itemsRebuilt_ = 0;
    std::vector<protocol::Record> rebuilt_;
    uint64_t version_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
