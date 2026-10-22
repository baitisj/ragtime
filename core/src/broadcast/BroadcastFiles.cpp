#include "ragtime/broadcast/BroadcastFiles.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "ragtime/files/SessionFiles.h"

namespace ragtime
{
namespace broadcast
{

namespace
{

using State = BroadcastFile::State;
using HostEvent = link::HostLink::Event;
using BurstState = link::HostLink::BurstState;

constexpr size_t MAX_PIECES = 0xFFFF;
// Receiving with nothing new this long shows Stalled.
constexpr uint64_t STALLED_MS = 2 * 60 * 1000;

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

// The first n bytes of a UTF-8 name, cut between characters.
std::string cutName(const std::string& name, size_t n)
{
    if (name.size() <= n) return name;
    size_t at = n;
    while (at > 0 && ((uint8_t)name[at] & 0xC0) == 0x80) at--;
    return name.substr(0, at);
}

uint16_t pieceCountFor(uint64_t size, uint16_t pieceBytes)
{
    if (pieceBytes == 0) return 0;
    uint64_t n = (size + pieceBytes - 1) / pieceBytes;
    return n > MAX_PIECES ? 0 : (uint16_t)n;
}

// How long piece index of a file is.
size_t pieceLength(uint64_t size, uint16_t pieceBytes, uint16_t index)
{
    uint64_t start = (uint64_t)index * pieceBytes;
    if (start >= size) return 0;
    return (size_t)std::min<uint64_t>(pieceBytes, size - start);
}

std::string countText(size_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string clockText(uint64_t ms)
{
    uint64_t s = ms / 1000;
    return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
}

} // namespace

bool BroadcastFile::live() const
{
    switch (state)
    {
        case State::Sending:
        case State::Repairing:
        case State::Heard:
        case State::Receiving:
        case State::Incomplete:
            return true;
        default:
            return false;
    }
}

const char* stateLabel(BroadcastFile::State state)
{
    switch (state)
    {
        case State::Sending:
            return "Sending";
        case State::Repairing:
            return "Repairing";
        case State::Sent:
            return "Sent";
        case State::Cancelled:
            return "Cancelled";
        case State::Heard:
            return "Heard";
        case State::Receiving:
            return "Receiving";
        case State::Ignored:
            return "Ignored";
        case State::Saved:
            return "Saved";
        case State::Incomplete:
            return "Incomplete";
        case State::Failed:
            return "Failed";
        case State::Expired:
            return "Expired";
    }
    return "";
}

BroadcastFiles::BroadcastFiles(link::HostLink& link, BroadcastChat& chat, Seals& seals)
    : link_(link)
    , chat_(chat)
    , seals_(seals)
{
    seals_.setFiles(this);
}

BroadcastFiles::~BroadcastFiles()
{
    seals_.setFiles(nullptr);
}

void BroadcastFiles::setAutoReceive(const std::string& folder)
{
    autoFolder_ = folder;
}

//-------------------------------------------------------------------------
// Ours
//-------------------------------------------------------------------------

FileEstimate BroadcastFiles::estimate(uint64_t size, const std::string& name, const std::string& mode) const
{
    return estimateFile(size, name, chat_.settings().callsign, findMode(link_.modes(), mode, link_.groupMode()));
}

uint64_t BroadcastFiles::send(const std::string& path, const std::string& mode, uint64_t nowMs, std::string& error)
{
    const std::string& call = chat_.settings().callsign;
    std::filesystem::path p = files::pathFromUtf8(path);
    std::string name = cutName(files::utf8FromPath(p.filename()), protocol::MAX_FILE_NAME);
    if (call.empty())
    {
        error = "Set your callsign first";
        return 0;
    }
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(p, ec);
    if (ec || size == 0)
    {
        error = name + " is empty or can't be read";
        return 0;
    }
    if (size > MAX_FILE_BYTES)
    {
        error = name + " is " + files::groupedNumber(size) + " bytes; a file over HF can be 300 KB at most";
        return 0;
    }
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
        error = name + " can't be read";
        return 0;
    }
    if (content.size() != size)
    {
        error = name + " changed while it was read";
        return 0;
    }

    FileEstimate est = estimate(size, name, mode);
    if (est.airSeconds > REFUSE_AIR_SECONDS)
    {
        error = name + " would take " + std::to_string((int)(est.airSeconds / 60)) + " min on the air in this gear; "
                + "a broadcast file may take two hours at most";
        return 0;
    }
    uint16_t pieces = pieceCountFor(size, (uint16_t)est.pieceBytes);
    if (pieces == 0)
    {
        error = name + " needs too many pieces in this mode";
        return 0;
    }

    protocol::FileAnnounce announce;
    announce.size = (uint32_t)size;
    announce.pieceBytes = (uint16_t)est.pieceBytes;
    announce.digest = protocol::digestOf(content);
    announce.name = name;

    Outgoing out;
    out.announce = chat_.record(announce, seconds(nowMs));
    out.content = std::move(content);
    out.pieceBytes = announce.pieceBytes;
    out.file.id = nextId_++;
    out.file.outgoing = true;
    out.file.from = call;
    out.file.seq = out.announce.header.seq;
    out.file.name = name;
    out.file.size = size;
    out.file.pieces = pieces;
    out.file.state = State::Sending;
    out.file.mode = mode;
    out.file.startedMs = nowMs;
    out.file.lastMs = nowMs;
    out.file.path = path;
    out.file.pieceMap.assign(pieces, files::PIECE_WAITING);
    out.rate.sample(0, nowMs);
    outgoing_.push_back(std::move(out));
    Outgoing& o = outgoing_.back();
    nextBurst(o, nowMs);
    emit(Event::Type::Sending, o.file.id);
    changed();
    return o.file.id;
}

void BroadcastFiles::nextBurst(Outgoing& out, uint64_t nowMs)
{
    link::ModeInfo info = findMode(link_.modes(), out.file.mode, link_.groupMode());
    std::vector<std::vector<uint8_t>> frames;
    std::vector<int> lengths;
    std::vector<uint16_t> pieces;
    bool first = out.lastBlock < 0;

    int block = out.next / ANNOUNCE_EVERY_PIECES;
    if (block > out.lastBlock)
    {
        frames.push_back(protocol::encodeFrame({out.announce}));
        lengths.push_back((int)frames.back().size());
        out.lastBlock = block;
    }
    uint32_t time = seconds(nowMs);
    while (out.next < out.file.pieces)
    {
        // A new block's announce leads its own burst.
        if (!frames.empty() && out.next % ANNOUNCE_EVERY_PIECES == 0 && out.next / ANNOUNCE_EVERY_PIECES > out.lastBlock)
            break;
        protocol::Record r;
        if (!piece(out.file.seq, out.next, time, r)) break;
        std::vector<uint8_t> frame = protocol::encodeFrame({r});
        lengths.push_back((int)frame.size());
        if (!frames.empty() && !fitsOneBurst(lengths, info)) break;
        frames.push_back(std::move(frame));
        pieces.push_back(out.next);
        out.next++;
    }
    if (frames.empty()) return;
    out.frames = frames;
    out.burstPieces = pieces;
    out.tries = 1;
    out.firstBurst = first;
    out.burst = link_.broadcast(frames, out.file.mode, nowMs);
    if (first) seals_.going({out.announce}, out.burst, out.file.mode, nowMs);
}

void BroadcastFiles::firstPassDone(Outgoing& out, uint64_t nowMs)
{
    (void)nowMs;
    out.file.state = State::Repairing;
    seals_.sealSoon();
    changed();
}

void BroadcastFiles::finishOutgoing(Outgoing& out, State state, uint64_t nowMs)
{
    if (out.burst != 0) link_.cancel(out.burst);
    out.burst = 0;
    out.file.state = state;
    out.file.lastMs = nowMs;
    out.content.clear();
    out.content.shrink_to_fit();
    if (!out.finished)
    {
        out.finished = true;
        files::FinishedFile f;
        f.via = files::Via::Broadcast;
        f.atMs = nowMs;
        f.outgoing = true;
        f.peer = files::BROADCAST_PEER;
        f.name = out.file.name;
        f.size = out.file.size;
        f.result = state == State::Sent ? files::FinishedFile::Result::Sent : files::FinishedFile::Result::Cancelled;
        f.path = out.file.path;
        finished_.push_back(std::move(f));
    }
    emit(Event::Type::Ended, out.file.id);
    changed();
}

bool BroadcastFiles::cancel(uint64_t id)
{
    Outgoing* out = findOutgoing(id);
    if (out == nullptr || !out->file.live()) return false;
    if (out->file.state == State::Sending)
    {
        // What has gone is still sealed; nothing more of it goes, and
        // requests for it bring nothing.
        finishOutgoing(*out, State::Cancelled, out->file.lastMs);
        seals_.sealSoon();
        return true;
    }
    return closeRepairs(id);
}

bool BroadcastFiles::keepOpen(uint64_t id, uint64_t untilMs)
{
    Outgoing* out = findOutgoing(id);
    if (out == nullptr || out->file.state != State::Repairing) return false;
    if (!seals_.keepOpen(out->file.seq, untilMs)) return false;
    changed();
    return true;
}

bool BroadcastFiles::closeRepairs(uint64_t id)
{
    Outgoing* out = findOutgoing(id);
    if (out == nullptr || out->file.state != State::Repairing) return false;
    seals_.closeNow(out->file.seq);
    finishOutgoing(*out, State::Sent, out->file.lastMs);
    return true;
}

//-------------------------------------------------------------------------
// SealedFiles
//-------------------------------------------------------------------------

uint16_t BroadcastFiles::pieceCount(uint32_t seq) const
{
    const Outgoing* out = outgoingBySeq(seq);
    return out != nullptr && !out->content.empty() ? out->file.pieces : 0;
}

bool BroadcastFiles::streaming(uint32_t seq) const
{
    const Outgoing* out = outgoingBySeq(seq);
    return out != nullptr && out->file.state == State::Sending;
}

bool BroadcastFiles::piece(uint32_t seq, uint16_t index, uint32_t time, protocol::Record& record)
{
    const Outgoing* out = outgoingBySeq(seq);
    if (out == nullptr || out->content.empty() || index >= out->file.pieces) return false;
    size_t start = (size_t)index * out->pieceBytes;
    size_t length = pieceLength(out->file.size, out->pieceBytes, index);
    protocol::FilePiece p;
    p.index = index;
    p.bytes.assign(out->content.begin() + (std::ptrdiff_t)start,
                   out->content.begin() + (std::ptrdiff_t)(start + length));
    record = chat_.record(std::move(p), time, seq);
    return true;
}

bool BroadcastFiles::wanted(const std::string& call, uint32_t seq, std::vector<uint16_t>& missing) const
{
    for (const Incoming& in : incoming_)
    {
        if (in.file.from != call || in.file.seq != seq) continue;
        if (!in.announced || in.file.complete) return false;
        if (in.file.state != State::Receiving && in.file.state != State::Incomplete) return false;
        missing.clear();
        for (uint16_t i = 0; i < in.file.pieces; i++)
        {
            if (in.pieces.count(i) == 0) missing.push_back(i);
        }
        return !missing.empty();
    }
    return false;
}

//-------------------------------------------------------------------------
// Theirs
//-------------------------------------------------------------------------

BroadcastFiles::Incoming* BroadcastFiles::incomingFor(const std::string& call, uint32_t seq)
{
    for (Incoming& in : incoming_)
    {
        if (in.file.from == call && in.file.seq == seq) return &in;
    }
    return nullptr;
}

void BroadcastFiles::heardAnnounce(const protocol::Record& record, uint64_t nowMs)
{
    const protocol::FileAnnounce* a = record.as<protocol::FileAnnounce>();
    const std::string& from = record.header.callsign;
    uint16_t pieces = pieceCountFor(a->size, a->pieceBytes);
    if (a->size == 0 || pieces == 0) return;

    Incoming* in = incomingFor(from, record.header.seq);
    if (in != nullptr && in->announced) return;
    if (in == nullptr)
    {
        incoming_.emplace_back();
        in = &incoming_.back();
        in->file.id = nextId_++;
        in->file.outgoing = false;
        in->file.from = from;
        in->file.seq = record.header.seq;
        in->file.state = State::Heard;
        in->file.startedMs = nowMs;
        in->file.lastMs = nowMs;
    }
    in->announced = true;
    in->announce = *a;
    in->file.name = files::safeFileName(a->name);
    in->file.size = a->size;
    in->file.pieces = pieces;
    in->file.pieceMap.assign(pieces, files::PIECE_WAITING);

    // Pieces heard before the announce that don't fit it go.
    for (auto it = in->pieces.begin(); it != in->pieces.end();)
    {
        bool fits = it->first < pieces && it->second.size() == pieceLength(a->size, a->pieceBytes, it->first);
        if (!fits) in->bytes -= it->second.size();
        it = fits ? std::next(it) : in->pieces.erase(it);
    }
    in->file.held = (uint16_t)in->pieces.size();

    // The same file from the same station, sent again: carry on from what
    // came the first time.
    for (auto other = incoming_.begin(); other != incoming_.end(); ++other)
    {
        if (&*other == in || other->file.from != from || !other->announced) continue;
        bool same = other->announce.size == a->size && other->announce.pieceBytes == a->pieceBytes
                    && other->announce.digest == a->digest;
        if (!same || other->file.complete) continue;
        if (other->file.state != State::Receiving && other->file.state != State::Incomplete) continue;
        for (auto& p : other->pieces)
        {
            if (in->pieces.count(p.first) == 0)
            {
                in->bytes += p.second.size();
                in->pieces.emplace(p.first, std::move(p.second));
            }
        }
        in->file.held = (uint16_t)in->pieces.size();
        in->file.state = State::Receiving;
        in->file.autoReceived = other->file.autoReceived;
        in->file.path = other->file.path;
        in->chosenPath = other->chosenPath;
        incoming_.erase(other);
        break;
    }

    if (a->size > MAX_FILE_BYTES)
    {
        emit(Event::Type::Announced, in->file.id);
        finishIncoming(*in, State::Failed, nowMs, "over 300 KB");
        return;
    }
    if (in->file.state == State::Heard && !autoFolder_.empty())
    {
        in->file.state = State::Receiving;
        in->file.autoReceived = true;
        in->file.path = files::utf8FromPath(files::pathFromUtf8(autoFolder_) / files::pathFromUtf8(in->file.name));
    }
    emit(Event::Type::Announced, in->file.id);
    changed();
    tryComplete(*in, nowMs);
}

void BroadcastFiles::heardPiece(const protocol::Record& record, uint64_t nowMs)
{
    const protocol::FilePiece* p = record.as<protocol::FilePiece>();
    const std::string& from = record.header.callsign;
    if (p->bytes.empty()) return;
    Incoming* in = incomingFor(from, record.header.seq);
    if (in == nullptr)
    {
        // Before its announce: kept, unnamed, until the announce comes.
        incoming_.emplace_back();
        in = &incoming_.back();
        in->file.id = nextId_++;
        in->file.outgoing = false;
        in->file.from = from;
        in->file.seq = record.header.seq;
        in->file.state = State::Heard;
        in->file.startedMs = nowMs;
    }
    State s = in->file.state;
    if (s != State::Heard && s != State::Receiving && s != State::Incomplete) return;
    if (in->announced)
    {
        if (p->index >= in->file.pieces
            || p->bytes.size() != pieceLength(in->announce.size, in->announce.pieceBytes, p->index))
            return;
    }
    else if (p->index >= MAX_PIECES)
    {
        return;
    }
    in->file.lastMs = nowMs;
    if (in->pieces.count(p->index) != 0) return;
    in->bytes += p->bytes.size();
    in->pieces.emplace(p->index, p->bytes);
    in->file.held = (uint16_t)in->pieces.size();
    if (in->file.state == State::Incomplete)
    {
        in->file.state = State::Receiving;
        in->file.expiresAtMs = 0;
    }
    in->rate.sample(in->bytes, nowMs);
    changed();
    tryComplete(*in, nowMs);
    limitSpool();
}

void BroadcastFiles::tryComplete(Incoming& in, uint64_t nowMs)
{
    if (!in.announced || in.file.complete || in.pieces.size() < in.file.pieces) return;
    std::vector<uint8_t> content;
    content.reserve(in.announce.size);
    for (const auto& p : in.pieces) content.insert(content.end(), p.second.begin(), p.second.end());
    if (content.size() != in.announce.size || protocol::digestOf(content) != in.announce.digest)
    {
        // Something in it is wrong: all of it again, once.
        in.pieces.clear();
        in.bytes = 0;
        in.file.held = 0;
        in.digestFailures++;
        if (in.digestFailures >= 2) finishIncoming(in, State::Failed, nowMs, "its digest didn't match");
        changed();
        return;
    }
    in.file.complete = true;
    changed();
    if (in.file.state != State::Receiving) return;
    std::string error;
    if (save(in, error))
    {
        finishIncoming(in, State::Saved, nowMs);
        emit(Event::Type::Saved, in.file.id);
    }
    else
    {
        finishIncoming(in, State::Failed, nowMs, error);
    }
}

bool BroadcastFiles::save(Incoming& in, std::string& error)
{
    std::filesystem::path target = files::pathFromUtf8(in.file.path);
    if (!in.chosenPath)
    {
        // Received without asking: never over a file already there.
        std::filesystem::path folder = target.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        target = files::freeSavePath(folder, in.file.name);
        if (target.empty())
        {
            error = "no free name for it in " + files::utf8FromPath(folder);
            return false;
        }
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    for (const auto& p : in.pieces) out.write((const char*)p.second.data(), (std::streamsize)p.second.size());
    out.close();
    if (!out)
    {
        error = "it couldn't be written to " + files::utf8FromPath(target);
        return false;
    }
    in.file.path = files::utf8FromPath(target);
    return true;
}

void BroadcastFiles::finishIncoming(Incoming& in, State state, uint64_t nowMs, const std::string& error)
{
    in.file.state = state;
    in.file.error = error;
    in.file.lastMs = nowMs;
    in.pieces.clear();
    in.bytes = 0;
    if (!in.finished && in.announced)
    {
        in.finished = true;
        files::FinishedFile f;
        f.via = files::Via::Broadcast;
        f.atMs = nowMs;
        f.outgoing = false;
        f.peer = in.file.from;
        f.name = in.file.name;
        f.size = in.file.size;
        using Result = files::FinishedFile::Result;
        switch (state)
        {
            case State::Saved:
                f.result = Result::Saved;
                f.path = in.file.path;
                break;
            case State::Ignored:
                f.result = Result::Declined;
                break;
            case State::Expired:
                f.result = Result::Expired;
                break;
            default:
                f.result = Result::Failed;
                f.reason = error;
                break;
        }
        finished_.push_back(std::move(f));
    }
    emit(Event::Type::Ended, in.file.id);
    changed();
}

bool BroadcastFiles::receive(uint64_t id, const std::string& path, uint64_t nowMs, std::string& error)
{
    Incoming* in = findIncoming(id);
    if (in == nullptr || !in->announced || !in->file.live())
    {
        error = "It is no longer here";
        return false;
    }
    if (!path.empty())
    {
        in->file.path = path;
        in->chosenPath = true;
    }
    else if (in->file.path.empty())
    {
        error = "Nowhere to save it";
        return false;
    }
    in->file.state = State::Receiving;
    in->file.expiresAtMs = 0;
    in->file.lastMs = nowMs;
    changed();
    if (in->file.complete)
    {
        // Every piece came while it waited: it is saved now.
        in->file.complete = false;
        tryComplete(*in, nowMs);
        return true;
    }
    seals_.askSoon(in->file.from, nowMs);
    return true;
}

bool BroadcastFiles::ignore(uint64_t id)
{
    Incoming* in = findIncoming(id);
    if (in == nullptr || !in->file.live()) return false;
    finishIncoming(*in, State::Ignored, in->file.lastMs);
    return true;
}

bool BroadcastFiles::remove(uint64_t id)
{
    for (auto it = outgoing_.begin(); it != outgoing_.end(); ++it)
    {
        if (it->file.id == id && !it->file.live())
        {
            outgoing_.erase(it);
            changed();
            return true;
        }
    }
    for (auto it = incoming_.begin(); it != incoming_.end(); ++it)
    {
        if (it->file.id == id && !it->file.live())
        {
            incoming_.erase(it);
            changed();
            return true;
        }
    }
    return false;
}

void BroadcastFiles::limitSpool()
{
    // Pieces of files nobody is receiving yet, oldest heard first.
    for (;;)
    {
        size_t total = 0;
        Incoming* oldest = nullptr;
        for (Incoming& in : incoming_)
        {
            if (in.file.state != State::Heard || in.pieces.empty()) continue;
            total += in.bytes;
            if (oldest == nullptr || in.file.lastMs < oldest->file.lastMs) oldest = &in;
        }
        if (total <= SPOOL_BYTES || oldest == nullptr) return;
        oldest->pieces.clear();
        oldest->bytes = 0;
        oldest->file.held = 0;
        changed();
    }
}

void BroadcastFiles::prune()
{
    auto finishedCount = [](const auto& list) {
        return (size_t)std::count_if(list.begin(), list.end(), [](const auto& e) { return !e.file.live(); });
    };
    while (finishedCount(outgoing_) > MAX_FINISHED_KEPT)
    {
        auto it = std::find_if(outgoing_.begin(), outgoing_.end(), [](const Outgoing& o) { return !o.file.live(); });
        outgoing_.erase(it);
    }
    while (finishedCount(incoming_) > MAX_FINISHED_KEPT)
    {
        auto it = std::find_if(incoming_.begin(), incoming_.end(), [](const Incoming& i) { return !i.file.live(); });
        incoming_.erase(it);
    }
}

//-------------------------------------------------------------------------
// Events, records and the clock
//-------------------------------------------------------------------------

void BroadcastFiles::onEvent(const HostEvent& event, uint64_t nowMs)
{
    if (event.type != HostEvent::Type::Burst) return;
    for (Outgoing& out : outgoing_)
    {
        if (out.burst == 0 || out.burst != event.id) continue;
        if (event.burstState == BurstState::Sent)
        {
            link::ModeInfo info = findMode(link_.modes(), out.file.mode, link_.groupMode());
            seals_.addAirtime(out.file.seq, burstSeconds(out.frames, info));
            for (uint16_t p : out.burstPieces)
            {
                if (out.file.pieceMap[p] != files::PIECE_HELD) out.file.held++;
                out.file.pieceMap[p] = files::PIECE_HELD;
            }
            out.burst = 0;
            out.file.lastMs = nowMs;
            out.nextMs = nowMs + FILE_BURST_GAP_MS;
            out.rate.sample(std::min<uint64_t>(out.file.size, (uint64_t)out.file.held * out.pieceBytes), nowMs);
        }
        else if (event.burstState == BurstState::NotSent)
        {
            if (out.tries < BURST_TRIES && out.file.state == State::Sending)
            {
                out.tries++;
                out.burst = link_.broadcast(out.frames, out.file.mode, nowMs);
                // The announce's own fate follows its first burst; once
                // that is retried, it follows this one.
                if (out.firstBurst)
                    seals_.going({out.announce}, out.burst, out.file.mode, nowMs);
                continue;
            }
            // Given up: its pieces are left to the repairs.
            for (uint16_t p : out.burstPieces) out.file.pieceMap[p] = files::PIECE_MISSED;
            out.burst = 0;
            out.nextMs = nowMs + FILE_BURST_GAP_MS;
        }
        else
        {
            continue;
        }
        if (out.file.state == State::Sending && out.next >= out.file.pieces && out.burst == 0)
            firstPassDone(out, nowMs);
        changed();
    }
}

void BroadcastFiles::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (from.empty() || from == chat_.settings().callsign) return;
    if (record.as<protocol::FileAnnounce>() != nullptr)
        heardAnnounce(record, nowMs);
    else if (record.as<protocol::FilePiece>() != nullptr)
        heardPiece(record, nowMs);
}

void BroadcastFiles::tick(uint64_t nowMs)
{
    for (Outgoing& out : outgoing_)
    {
        if (out.file.state == State::Sending)
        {
            if (out.burst == 0 && out.next < out.file.pieces && nowMs >= out.nextMs) nextBurst(out, nowMs);
        }
        else if (out.file.state == State::Repairing)
        {
            Seals::SealView seal = seals_.sealOf(out.file.seq, nowMs);
            if (seal.sealed) out.file.sealed = true;
            if (out.file.sealed && !seal.open) finishOutgoing(out, State::Sent, nowMs);
        }
    }
    for (auto it = incoming_.begin(); it != incoming_.end();)
    {
        Incoming& in = *it;
        if (!in.announced && in.file.live() && nowMs >= in.file.lastMs + INCOMPLETE_MS)
        {
            // Pieces of a file never announced: dropped without a word.
            it = incoming_.erase(it);
            changed();
            continue;
        }
        if (in.file.state == State::Receiving && !in.file.complete && seals_.sealHeard(in.file.from, in.file.seq)
            && nowMs >= in.file.lastMs + INCOMPLETE_MS)
        {
            in.file.state = State::Incomplete;
            in.file.expiresAtMs = nowMs + KEEP_MS;
            changed();
        }
        else if (in.file.state == State::Incomplete && nowMs >= in.file.expiresAtMs)
        {
            finishIncoming(in, State::Failed, nowMs,
                           std::to_string(in.file.held) + " of " + std::to_string(in.file.pieces) + " pieces came");
        }
        else if (in.file.state == State::Heard && in.announced && nowMs >= in.file.startedMs + KEEP_MS)
        {
            finishIncoming(in, State::Expired, nowMs);
        }
        ++it;
    }
    prune();
}

//-------------------------------------------------------------------------
// What the screen shows
//-------------------------------------------------------------------------

BroadcastFiles::Outgoing* BroadcastFiles::findOutgoing(uint64_t id)
{
    for (Outgoing& out : outgoing_)
    {
        if (out.file.id == id) return &out;
    }
    return nullptr;
}

BroadcastFiles::Outgoing* BroadcastFiles::outgoingBySeq(uint32_t seq)
{
    for (Outgoing& out : outgoing_)
    {
        if (out.file.seq == seq) return &out;
    }
    return nullptr;
}

const BroadcastFiles::Outgoing* BroadcastFiles::outgoingBySeq(uint32_t seq) const
{
    for (const Outgoing& out : outgoing_)
    {
        if (out.file.seq == seq) return &out;
    }
    return nullptr;
}

BroadcastFiles::Incoming* BroadcastFiles::findIncoming(uint64_t id)
{
    for (Incoming& in : incoming_)
    {
        if (in.file.id == id) return &in;
    }
    return nullptr;
}

BroadcastFile BroadcastFiles::view(const Outgoing& out, uint64_t nowMs) const
{
    BroadcastFile f = out.file;
    if (f.state == State::Repairing)
    {
        Seals::SealView seal = seals_.sealOf(f.seq, nowMs);
        f.sealed = seal.sealed;
        f.repairsUntilMs = seal.openUntilMs;
        f.keptOpen = seal.keptOpen;
        f.askers = seal.askers;
    }
    return f;
}

BroadcastFile BroadcastFiles::view(const Incoming& in, uint64_t nowMs) const
{
    (void)nowMs;
    BroadcastFile f = in.file;
    if (!f.live()) return f;
    f.sealed = seals_.sealHeard(f.from, f.seq);
    f.asking = seals_.asking(f.from) && f.state == State::Receiving;
    if (in.announced)
    {
        // Missed: its turn has gone by (a later piece is here, or the seal).
        uint16_t last = in.pieces.empty() ? 0 : in.pieces.rbegin()->first;
        for (uint16_t i = 0; i < f.pieces; i++)
        {
            if (in.pieces.count(i) != 0)
                f.pieceMap[i] = files::PIECE_HELD;
            else if (f.sealed || (!in.pieces.empty() && i < last))
                f.pieceMap[i] = files::PIECE_MISSED;
            else
                f.pieceMap[i] = files::PIECE_WAITING;
        }
        if (f.state == State::Heard) f.expiresAtMs = f.startedMs + KEEP_MS;
    }
    return f;
}

std::vector<BroadcastFile> BroadcastFiles::files(uint64_t nowMs) const
{
    std::vector<BroadcastFile> all;
    for (const Outgoing& out : outgoing_) all.push_back(view(out, nowMs));
    for (const Incoming& in : incoming_)
    {
        if (in.announced) all.push_back(view(in, nowMs));
    }
    return all;
}

bool BroadcastFiles::find(uint64_t id, uint64_t nowMs, BroadcastFile& out) const
{
    for (const Outgoing& o : outgoing_)
    {
        if (o.file.id != id) continue;
        out = view(o, nowMs);
        return true;
    }
    for (const Incoming& i : incoming_)
    {
        if (i.file.id != id) continue;
        out = view(i, nowMs);
        return true;
    }
    return false;
}

std::vector<files::FileRow> BroadcastFiles::rows(uint64_t nowMs) const
{
    using Look = files::FileRow::Look;
    std::vector<files::FileRow> rows;
    auto base = [](const BroadcastFile& f) {
        files::FileRow row;
        row.via = files::Via::Broadcast;
        row.id = f.id;
        row.atMs = f.startedMs;
        row.name = f.name;
        row.size = f.size;
        row.outgoing = f.outgoing;
        row.peer = f.outgoing ? std::string(files::BROADCAST_PEER) : f.from;
        row.pieces = f.pieceMap;
        return row;
    };
    for (const Outgoing& out : outgoing_)
    {
        if (!out.file.live()) continue;
        BroadcastFile f = view(out, nowMs);
        files::FileRow row = base(f);
        uint64_t sent = std::min<uint64_t>(f.size, (uint64_t)f.held * out.pieceBytes);
        row.done = sent;
        row.handed = sent;
        if (f.state == State::Sending)
        {
            row.state = "Sending, " + std::to_string(f.held) + " of " + countText(f.pieces, "piece", "pieces");
            row.look = Look::Moving;
            row.bytesPerSecond = out.rate.rate();
            row.finishAtMs = out.rate.finishAtMs(f.size, nowMs);
        }
        else
        {
            row.state = !f.sealed ? std::string("Sealing")
                                  : f.askers == 0 ? std::string("Repairing, no requests yet")
                                                  : "Repairing, " + countText(f.askers, "station", "stations") + " asked";
            row.look = Look::Repairing;
            row.expiresAtMs = f.repairsUntilMs;
        }
        rows.push_back(std::move(row));
    }
    for (const Incoming& in : incoming_)
    {
        if (!in.announced || !in.file.live()) continue;
        BroadcastFile f = view(in, nowMs);
        files::FileRow row = base(f);
        row.done = std::min<uint64_t>(f.size, in.bytes);
        row.handed = row.done;
        size_t missing = (size_t)f.pieces - in.pieces.size();
        if (f.state == State::Heard)
        {
            row.state = "Heard: Receive or Ignore";
            row.look = Look::Waiting;
            row.expiresAtMs = f.expiresAtMs;
        }
        else if (f.state == State::Incomplete)
        {
            row.state = "Part kept, " + std::to_string(in.pieces.size()) + " of " + countText(f.pieces, "piece", "pieces");
            row.look = Look::Kept;
            row.expiresAtMs = f.expiresAtMs;
        }
        else if (f.sealed && missing > 0)
        {
            row.state = (f.asking ? "Asking for " : "Repairing, ") + countText(missing, "piece", "pieces")
                        + (f.asking ? "" : " missing");
            row.look = Look::Repairing;
        }
        else if (nowMs >= in.file.lastMs + STALLED_MS)
        {
            row.state = "Stalled " + clockText(nowMs - in.file.lastMs);
            row.look = Look::Stalled;
        }
        else
        {
            row.state = "Receiving";
            row.look = Look::Moving;
            row.bytesPerSecond = in.rate.rate();
            row.finishAtMs = in.rate.finishAtMs(f.size, nowMs);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<files::FinishedFile> BroadcastFiles::takeFinished()
{
    std::vector<files::FinishedFile> out;
    out.swap(finished_);
    return out;
}

std::vector<BroadcastFiles::Event> BroadcastFiles::takeEvents()
{
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

} // namespace broadcast
} // namespace ragtime
