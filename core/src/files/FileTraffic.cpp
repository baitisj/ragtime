#include "ragtime/files/FileTraffic.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ragtime/broadcast/Airtime.h"

namespace ragtime
{
namespace files
{

namespace
{

using State = FileTransfer::State;

FinishedFile::Result resultOf(State state)
{
    using R = FinishedFile::Result;
    switch (state)
    {
        case State::Delivered:
            return R::Sent;
        case State::Saved:
            return R::Saved;
        case State::Declined:
            return R::Declined;
        case State::Cancelled:
            return R::Cancelled;
        case State::CancelledThere:
            return R::CancelledThere;
        case State::Expired:
            return R::Expired;
        case State::NotTaken:
            return R::NotTaken;
        default:
            return R::Failed;
    }
}

} // namespace

FileTraffic::FileTraffic(SessionFiles& files, FileHistory& history) : files_(files), history_(history)
{
}

//-------------------------------------------------------------------------
// The session's side
//-------------------------------------------------------------------------

void FileTraffic::greeted(const std::string& peer, const protocol::Hello& hello)
{
    if (peer != peer_)
    {
        peer_ = peer;
        acked_ = 0;
        session_.reset();
    }
    up_ = greeted_ = true;
    takes_ = (hello.takes & protocol::TAKES_SESSION_FILES) != 0;
    if (!takes_) files_.notTaken(peer);
    changes_++;
}

void FileTraffic::onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs)
{
    files_.onRecord(peer, record, nowMs);
    collect(nowMs);
}

void FileTraffic::arriving(const std::string& peer, uint32_t offer, uint32_t offset, const uint8_t* data,
                           size_t bytes)
{
    files_.arriving(peer, offer, offset, data, bytes);
}

uint64_t FileTraffic::burstBytes(const link::ModeInfo& mode)
{
    if (!mode.valid()) return 0;
    // Codewords past the first cost the same time each; as many as fit.
    double longest = broadcast::isCpmMode(mode) ? FSK_BURST_SECONDS : OFDM_BURST_SECONDS;
    int codewords = mode.maxCodewords;
    if (mode.maxCodewords > 1 && mode.secondsAtMax > mode.secondsAtOne)
    {
        double each = (mode.secondsAtMax - mode.secondsAtOne) / (mode.maxCodewords - 1);
        double more = std::floor((longest - mode.secondsAtOne) / each);
        codewords = 1 + (int)std::max(0.0, std::min(more, (double)(mode.maxCodewords - 1)));
    }
    return (uint64_t)mode.bytesPerCodeword * (uint64_t)codewords;
}

uint64_t FileTraffic::window() const
{
    if (cuts_) return MAX_WINDOW;
    uint64_t burst = burstBytes(mode_);
    if (burst > 0) return std::max(MIN_WINDOW, std::min(MAX_WINDOW, WINDOW_BURSTS * burst));
    double rate = session_.rate();
    if (!(rate > 0)) return START_WINDOW;
    uint64_t w = (uint64_t)std::llround(rate * (double)WINDOW_MS / 1000.0);
    return std::max(MIN_WINDOW, std::min(MAX_WINDOW, w));
}

size_t FileTraffic::pieceBytes() const
{
    if (!cuts_) return std::max(MIN_PIECE, std::min(SessionFiles::PIECE_BYTES, (size_t)(window() / 2)));
    uint64_t burst = burstBytes(mode_);
    if (burst == 0) return CUT_PIECE;
    return std::max(MIN_PIECE, std::min(SessionFiles::PIECE_BYTES, (size_t)(burst / 2)));
}

void FileTraffic::write(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset,
                        uint64_t nowMs, std::vector<uint8_t>& out)
{
    if (peer != peer_)
    {
        peer_ = peer;
        greeted_ = takes_ = false;
        acked_ = 0;
        session_.reset();
    }
    up_ = true;
    // A station that takes no files is never offered one.
    if (greeted_ && !takes_) files_.notTaken(peer);
    std::vector<uint8_t> records = files_.takeRecords(peer, numbering, nowMs, greeted_ && takes_);
    out.insert(out.end(), records.begin(), records.end());

    // File bytes, up to the window.
    uint64_t w = window();
    size_t piece = pieceBytes();
    for (;;)
    {
        uint64_t at = streamOffset + out.size();
        uint64_t waiting = at > acked_ ? at - acked_ : 0;
        if (waiting >= w) break;
        size_t room = (size_t)std::min<uint64_t>(piece, w - waiting);
        if (room < MIN_PIECE / 2) break;
        if (!files_.takePiece(peer, numbering, at, nowMs, out, room)) break;
    }
    collect(nowMs);
}

void FileTraffic::acknowledged(const std::string& peer, uint64_t streamAcked)
{
    if (peer != peer_) return;
    acked_ = std::max(acked_, streamAcked);
    files_.acknowledged(peer, streamAcked);
}

void FileTraffic::leaving(const std::string& peer, protocol::Numbering& numbering, uint64_t nowMs,
                          std::vector<uint8_t>& out)
{
    // What is under way is cancelled, so the far end hears it; a file still
    // waiting its turn waits for the next session.
    for (const FileTransfer& t : files_.transfers())
    {
        if (t.peer == peer && t.live() && t.state != State::Waiting) files_.cancel(t.id);
    }
    std::vector<uint8_t> records = files_.takeRecords(peer, numbering, nowMs, false);
    out.insert(out.end(), records.begin(), records.end());
    collect(nowMs);
}

void FileTraffic::ended(const std::string& peer, uint64_t nowMs)
{
    files_.sessionEnded(peer, nowMs);
    seenCuts_ = files_.cutsWanted();
    if (peer == peer_)
    {
        up_ = greeted_ = takes_ = false;
        acked_ = 0;
        session_.reset();
    }
    collect(nowMs);
}

bool FileTraffic::wantsCut(const std::string& peer)
{
    if (peer != peer_ || files_.cutsWanted() == seenCuts_) return false;
    seenCuts_ = files_.cutsWanted();
    return true;
}

void FileTraffic::cut(const std::string& peer, uint64_t streamFrom)
{
    files_.cut(peer, streamFrom);
}

bool FileTraffic::busy(const std::string& peer) const
{
    for (const FileTransfer& t : files_.transfers())
    {
        if (t.peer == peer && t.live() && t.state != State::Waiting) return true;
    }
    return false;
}

bool FileTraffic::takesFiles(const std::string& peer) const
{
    return peer == peer_ && greeted_ && takes_;
}

//-------------------------------------------------------------------------
// The clock and the log
//-------------------------------------------------------------------------

void FileTraffic::tick(uint64_t nowMs)
{
    files_.tick(nowMs);
    sample(nowMs);
    collect(nowMs);
}

void FileTraffic::update(uint64_t nowMs)
{
    collect(nowMs);
}

void FileTraffic::sample(uint64_t nowMs)
{
    if (!peer_.empty() && greeted_) session_.sample(acked_, nowMs);
    std::set<uint64_t> live;
    for (const FileTransfer& t : files_.transfers())
    {
        if (t.state != State::Sending && t.state != State::Receiving) continue;
        live.insert(t.id);
        meters_[t.id].sample(t.done, nowMs);
        started_.emplace(t.id, Start{nowMs, t.done});
    }
    for (auto it = meters_.begin(); it != meters_.end();)
        it = live.count(it->first) != 0 ? std::next(it) : meters_.erase(it);
}

void FileTraffic::collect(uint64_t nowMs)
{
    if (files_.changes() == seenChanges_) return;
    seenChanges_ = files_.changes();
    std::set<uint64_t> present;
    for (const FileTransfer& t : files_.transfers())
    {
        present.insert(t.id);
        if (!t.outgoing && announced_.insert(t.id).second)
        {
            Event e;
            e.type = Event::Type::Offered;
            e.id = t.id;
            e.peer = t.peer;
            e.autoAccepted = t.autoAccepted;
            events_.push_back(e);
        }
        if (t.live() || !recorded_.insert(t.id).second) continue;

        FinishedFile f;
        f.via = Via::Session;
        f.atMs = nowMs;
        f.outgoing = t.outgoing;
        f.peer = t.peer;
        f.name = t.name;
        f.size = t.size;
        f.result = resultOf(t.state);
        f.reason = t.error;
        if (t.state == State::FailedThere && f.reason.empty())
            f.reason = t.outgoing ? t.peer + " couldn't save it" : t.peer + " couldn't read it";
        if (t.outgoing || t.state == State::Saved) f.path = t.path;
        f.picture = t.picture;
        // A picture stopped part way: as far as it went.
        uint64_t crossed = t.enough ? std::min(t.done, t.size) : t.size;
        if (t.enough && crossed < t.size) f.enough = std::max<uint64_t>(1, crossed);
        auto start = started_.find(t.id);
        if (start != started_.end() && (t.state == State::Saved || t.state == State::Delivered) &&
            crossed > start->second.done && nowMs > start->second.atMs)
        {
            // A part kept from before isn't counted: only what crossed now.
            f.movedBytes = crossed - start->second.done;
            f.movedMs = nowMs - start->second.atMs;
        }
        Event e;
        e.type = Event::Type::Finished;
        e.id = t.id;
        e.peer = t.peer;
        e.finished = f;
        e.partKept = t.partKept;
        if (t.partKept)
            files_.setKeptEnding(t.id, f);
        else
            e.historyId = history_.add(std::move(f));
        events_.push_back(e);
        meters_.erase(t.id);
    }
    // Files whose kept parts went unused are done now.
    for (FinishedFile& f : files_.takeEnded())
    {
        f.atMs = nowMs;
        history_.add(std::move(f));
    }
    // Forget what the engine has let go of.
    for (std::set<uint64_t>* ids : {&recorded_, &announced_})
    {
        for (auto it = ids->begin(); it != ids->end();)
            it = present.count(*it) != 0 ? std::next(it) : ids->erase(it);
    }
    for (auto it = started_.begin(); it != started_.end();)
        it = present.count(it->first) != 0 && recorded_.count(it->first) == 0 ? std::next(it) : started_.erase(it);
    changes_++;
}

std::vector<FileTraffic::Event> FileTraffic::takeEvents()
{
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

//-------------------------------------------------------------------------
// The rows
//-------------------------------------------------------------------------

std::string FileTraffic::stateText(const FileTransfer& t, uint64_t nowMs, FileRow::Look& look) const
{
    look = FileRow::Look::Waiting;
    switch (t.state)
    {
        case State::Waiting:
            return up_ && t.peer == peer_ && (takes_ || !greeted_) ? "Queued" : "Waiting for a session with " + t.peer;
        case State::Offered:
            return "Waiting for answer";
        case State::Asking:
            return "Waiting for your answer";
        case State::Sending:
        case State::Receiving:
        {
            look = FileRow::Look::Moving;
            auto meter = meters_.find(t.id);
            uint64_t moved = meter != meters_.end() ? meter->second.lastMovedMs() : 0;
            if (moved != 0 && nowMs >= moved + STALL_MS)
            {
                look = FileRow::Look::Stalled;
                return "Stalled " + session::clockLength(nowMs - moved);
            }
            // QRL: the link is busy with this file (Jeff's word for it).
            if (t.outgoing && t.done >= t.size) return "QRL, " + t.peer + " is checking it";
            return "QRL";
        }
        default:
            return "";
    }
}

FileRow FileTraffic::row(const FileTransfer& t, uint64_t nowMs) const
{
    FileRow r;
    r.via = Via::Session;
    r.id = t.id;
    r.atMs = t.atMs;
    r.name = t.name;
    r.size = t.size;
    r.done = t.done;
    r.handed = std::max(t.done, t.handed);
    r.outgoing = t.outgoing;
    r.peer = t.peer;
    r.state = stateText(t, nowMs, r.look);
    if (t.expiresInMs != 0) r.expiresAtMs = nowMs + t.expiresInMs;
    auto meter = meters_.find(t.id);
    if (meter != meters_.end() && r.look == FileRow::Look::Moving)
    {
        r.bytesPerSecond = meter->second.rate();
        r.finishAtMs = meter->second.finishAtMs(t.size, nowMs);
    }
    return r;
}

std::vector<FileRow> FileTraffic::rows(uint64_t nowMs) const
{
    std::vector<FileRow> out;
    for (const FileTransfer& t : files_.transfers(nowMs))
    {
        if (t.live()) out.push_back(row(t, nowMs));
    }
    for (const KeptPart& k : files_.keptParts())
    {
        FileRow r;
        r.via = Via::Session;
        r.id = k.id;
        r.atMs = k.keptAtMs;
        r.name = k.name;
        r.size = k.size;
        r.done = k.bytes;
        r.handed = std::max(k.bytes, k.handed);
        r.outgoing = k.outgoing;
        r.peer = k.peer;
        r.state = partStateText(k);
        r.look = FileRow::Look::Kept;
        r.expiresAtMs = k.keepUntilMs;
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace files
} // namespace ragtime
