#include "ragtime/broadcast/BroadcastFiles.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ragtime/broadcast/BroadcastParts.h"
#include "ragtime/files/Parity.h"
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

// The status bar's line for a burst of a file's pieces: "map.png, 5-8 of 30".
std::string fileLabel(const BroadcastFile& file, const std::vector<uint16_t>& pieces)
{
    std::string label = file.name;
    if (pieces.empty() || file.pieces == 0) return label;
    label += ", " + std::to_string(pieces.front() + 1);
    if (pieces.back() != pieces.front()) label += "\xe2\x80\x93" + std::to_string(pieces.back() + 1);
    return label + " of " + std::to_string(file.pieces);
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

std::string callsText(const std::vector<std::string>& calls)
{
    const size_t named = 2;
    std::string text;
    for (size_t i = 0; i < calls.size() && i < named; i++)
    {
        if (i > 0) text += calls.size() == 2 ? " and " : ", ";
        text += calls[i];
    }
    if (calls.size() > named) text += " and " + std::to_string(calls.size() - named) + " more";
    return text;
}

std::string repairText(const BroadcastFile& f)
{
    if (f.roundPieces > 0)
    {
        std::string to = f.repairingFor.empty() ? std::string() : " to " + callsText(f.repairingFor);
        return "sending repairs" + to + ", " + std::to_string(f.roundSent) + " of "
               + countText(f.roundPieces, "piece", "pieces");
    }
    std::string text = !f.askedBy.empty() ? callsText(f.askedBy) + " asked"
                       : f.askers != 0    ? countText(f.askers, "station", "stations") + " asked"
                                          : std::string("no requests yet");
    if (f.repairPieces > 0) text += ", " + countText(f.repairPieces, "repair piece", "repair pieces") + " sent";
    return text;
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
    writeParts();
    seals_.setFiles(nullptr);
}

void BroadcastFiles::setAutoReceive(const std::string& folder)
{
    autoFolder_ = folder;
}

void BroadcastFiles::setAutoPictures(const std::string& folder, uint64_t maxBytes)
{
    pictureFolder_ = folder;
    pictureMax_ = maxBytes;
}

bool BroadcastFiles::pictureBytes(uint64_t id, std::vector<uint8_t>& out) const
{
    out.clear();
    auto readFile = [&out](const std::string& path, uint64_t size) {
        std::ifstream in(files::pathFromUtf8(path), std::ios::binary);
        if (!in) return false;
        out.resize((size_t)std::min<uint64_t>(size, MAX_FILE_BYTES));
        in.read((char*)out.data(), (std::streamsize)out.size());
        out.resize((size_t)in.gcount());
        return !out.empty();
    };
    for (const Outgoing& o : outgoing_)
    {
        if (o.file.id != id) continue;
        if (!o.content.empty())
        {
            out = o.content;
            return true;
        }
        return readFile(o.file.path, o.file.size);
    }
    for (const Incoming& i : incoming_)
    {
        if (i.file.id != id || !i.announced) continue;
        if (i.file.state == State::Saved) return readFile(i.file.path, i.file.size);
        for (uint16_t p = 0; p < i.file.pieces; p++)
        {
            auto it = i.pieces.find(p);
            if (it == i.pieces.end()) break;
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        return !out.empty();
    }
    return false;
}

//-------------------------------------------------------------------------
// Ours
//-------------------------------------------------------------------------

FileEstimate BroadcastFiles::estimate(uint64_t size, const std::string& name, const std::string& mode) const
{
    return estimateFile(size, name, chat_.settings().callsign, findMode(link_.modes(), mode, link_.groupMode()));
}

uint64_t BroadcastFiles::send(const std::string& path, const std::string& mode, uint64_t nowMs, std::string& error,
                              bool picture)
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
    announce.kind = picture ? protocol::pictureKindOf(content.data(), content.size()) : protocol::FILE_PLAIN;

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
    out.file.picture = picture;
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
    out.burst = link_.broadcast(frames, out.file.mode, nowMs, fileLabel(out.file, out.burstPieces), true);
    if (first) seals_.going({out.announce}, out.burst, out.file.mode, nowMs);
}

void BroadcastFiles::firstPassDone(Outgoing& out, uint64_t nowMs)
{
    out.firstPassMs = nowMs;
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
    out.slices.clear();
    out.slices.shrink_to_fit();
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
        f.picture = out.file.picture;
        // The first pass, start to end: repairs come later, sometimes hours.
        if (state == State::Sent && out.firstPassMs > out.file.startedMs)
        {
            f.movedBytes = out.file.size;
            f.movedMs = out.firstPassMs - out.file.startedMs;
        }
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
        if (out->file.held == 0)
        {
            // Nothing of it went: nobody can ask for it.
            finishOutgoing(*out, State::Cancelled, out->file.lastMs);
            seals_.sealSoon();
            return true;
        }
        // The first pass ends here. What went is sealed, and a fresh
        // request on the group for the rest is answered there.
        if (out->burst != 0) link_.cancel(out->burst);
        out->burst = 0;
        out->file.stopped = true;
        firstPassDone(*out, out->file.lastMs);
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
    out->capped = false;
    finishOutgoing(*out, out->file.stopped ? State::Cancelled : State::Sent, out->file.lastMs);
    return true;
}

bool BroadcastFiles::reopen(uint64_t id, uint64_t untilMs, uint64_t nowMs, std::string& error)
{
    Outgoing* out = findOutgoing(id);
    if (out == nullptr || (out->file.state != State::Sent && out->file.state != State::Cancelled))
    {
        error = "It isn't a finished broadcast of ours";
        return false;
    }
    if (!seals_.sealOf(out->file.seq, nowMs).sealed)
    {
        error = "Its seal is over 2 hours old, or from before Ragtime last started: broadcast it again instead";
        return false;
    }
    const protocol::FileAnnounce* a = out->announce.as<protocol::FileAnnounce>();
    std::ifstream in(files::pathFromUtf8(out->file.path), std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.is_open() || (!in.good() && !in.eof()))
    {
        error = out->file.name + " can't be read";
        return false;
    }
    if (a == nullptr || content.size() != out->file.size || protocol::digestOf(content) != a->digest)
    {
        error = out->file.name + " has changed since it went: broadcast it again instead";
        return false;
    }
    if (!seals_.reopen(out->file.seq, untilMs, nowMs))
    {
        error = "Its seal is no longer held: broadcast it again instead";
        return false;
    }
    out->content = std::move(content);
    out->capped = false;
    out->inRound = false;
    out->file.state = State::Repairing;
    out->file.sealed = true;
    out->file.lastMs = nowMs;
    emit(Event::Type::Reopened, out->file.id);
    changed();
    return true;
}

uint64_t BroadcastFiles::findSent(const std::string& path, uint64_t size) const
{
    // The newest, should it have gone more than once.
    for (auto it = outgoing_.rbegin(); it != outgoing_.rend(); ++it)
    {
        if (it->file.path == path && it->file.size == size && !it->file.live()) return it->file.id;
    }
    return 0;
}

bool BroadcastFiles::setRepairGear(uint64_t id, const std::string& mode)
{
    Outgoing* out = findOutgoing(id);
    if (out == nullptr || !out->file.live()) return false;
    seals_.setRepairGear(out->file.seq, mode);
    changed();
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

bool BroadcastFiles::stopped(uint32_t seq) const
{
    const Outgoing* out = outgoingBySeq(seq);
    return out != nullptr && out->file.stopped;
}

bool BroadcastFiles::streaming(uint32_t seq) const
{
    const Outgoing* out = outgoingBySeq(seq);
    return out != nullptr && out->file.state == State::Sending;
}

bool BroadcastFiles::piece(uint32_t seq, uint16_t index, uint32_t time, protocol::Record& record)
{
    Outgoing* out = outgoingBySeq(seq);
    if (out == nullptr || out->content.empty() || out->file.pieces == 0) return false;
    protocol::FilePiece p;
    p.index = index;
    if (index < out->file.pieces)
    {
        size_t start = (size_t)index * out->pieceBytes;
        size_t length = pieceLength(out->file.size, out->pieceBytes, index);
        p.bytes.assign(out->content.begin() + (std::ptrdiff_t)start,
                       out->content.begin() + (std::ptrdiff_t)(start + length));
    }
    else
    {
        // A parity piece, over all the pieces.
        if (out->slices.empty())
        {
            for (uint16_t i = 0; i < out->file.pieces; i++)
            {
                size_t start = (size_t)i * out->pieceBytes;
                size_t length = pieceLength(out->file.size, out->pieceBytes, i);
                out->slices.emplace_back(out->content.begin() + (std::ptrdiff_t)start,
                                         out->content.begin() + (std::ptrdiff_t)(start + length));
            }
        }
        std::vector<files::parity::Symbol> data;
        for (uint16_t i = 0; i < out->file.pieces; i++) data.push_back({i, &out->slices[i]});
        p.bytes = files::parity::parityOf(index, data, files::parity::symbolLength(out->pieceBytes));
        if (p.bytes.empty()) return false;
    }
    record = chat_.record(std::move(p), time, seq);
    return true;
}

double BroadcastFiles::lossOf(const Incoming& in) const
{
    // The last round's, when anything came of it; else the last known;
    // else the first pass's, counted from the first piece we heard (a
    // station that tuned in part way didn't lose what went before). A
    // round's loss is of the parity pieces that went, from the first heard
    // to the last: a round the sender's airtime cap cut short sent fewer
    // than were asked for, and those never sent weren't lost (Jeff's
    // AG7EW-KC2G test, 2026-10-25: 4 of 21 came in a capped round, read as
    // 80% lost, and the next request asked for 41).
    if (in.parityFrom != 0 && in.parityHeard != 0 && in.parityTop >= in.parityFrom)
    {
        uint32_t span = std::max((uint32_t)(in.parityTop - in.parityFrom) + 1, in.parityHeard);
        return 1.0 - (double)in.parityHeard / (double)span;
    }
    if (in.askedWant != 0 && in.came != 0)
        return 1.0 - std::min(1.0, (double)in.came / (double)in.askedWant);
    if (in.loss >= 0.0) return in.loss;
    if (in.pieces.empty()) return 0.5;
    size_t span = (size_t)in.file.pieces - in.pieces.begin()->first;
    return span == 0 ? 0.0 : 1.0 - std::min(1.0, (double)in.pieces.size() / (double)span);
}

bool BroadcastFiles::wanted(const std::string& call, uint32_t seq, uint32_t& need, uint32_t& want) const
{
    for (const Incoming& in : incoming_)
    {
        if (in.file.from != call || in.file.seq != seq) continue;
        if (!in.announced || in.file.complete) return false;
        if (in.file.state != State::Receiving) return false;
        size_t held = in.pieces.size() + in.parity.size();
        if (held >= in.file.pieces) return false;
        need = (uint32_t)(in.file.pieces - held);
        want = files::parity::wantFor(need, lossOf(in));
        return true;
    }
    return false;
}

void BroadcastFiles::asked(const std::string& call, uint32_t seq, uint32_t want)
{
    Incoming* in = incomingFor(call, seq);
    if (in == nullptr) return;
    in->loss = lossOf(*in);
    in->askedWant = want;
    in->came = 0;
    // A sender's parity pieces count up from its piece count.
    if (in->parityFrom == 0) in->parityFrom = in->parityTop != 0 ? (uint16_t)(in->parityTop + 1) : in->file.pieces;
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
    in->announceRecord = record;
    in->file.name = files::safeFileName(a->name);
    in->file.size = a->size;
    in->file.picture = protocol::isPictureKind(a->kind);
    in->file.pieces = pieces;
    in->file.pieceMap.assign(pieces, files::PIECE_WAITING);

    // Pieces heard before the announce: past its piece count, parity
    // pieces; any that don't fit it go.
    for (auto it = in->pieces.begin(); it != in->pieces.end();)
    {
        bool fits = it->first < pieces && it->second.size() == pieceLength(a->size, a->pieceBytes, it->first);
        if (fits)
        {
            ++it;
            continue;
        }
        in->bytes -= it->second.size();
        if (it->first >= pieces && it->second.size() == files::parity::symbolLength(a->pieceBytes))
        {
            in->parityBytes += it->second.size();
            in->parity.emplace(it->first, std::move(it->second));
        }
        it = in->pieces.erase(it);
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
        // Parity pieces with the same index over the same file are the same.
        for (auto& p : other->parity)
        {
            if (in->parity.count(p.first) == 0)
            {
                in->parityBytes += p.second.size();
                in->parity.emplace(p.first, std::move(p.second));
            }
        }
        in->file.held = (uint16_t)in->pieces.size();
        in->file.state = State::Receiving;
        in->file.autoReceived = other->file.autoReceived;
        in->file.path = other->file.path;
        in->chosenPath = other->chosenPath;
        dropPart(*other);
        incoming_.erase(other);
        break;
    }

    if (a->size > MAX_FILE_BYTES)
    {
        emit(Event::Type::Announced, in->file.id);
        finishIncoming(*in, State::Failed, nowMs, "over 300 KB");
        return;
    }
    // A picture small enough shows as it comes; other files wait for
    // Receive unless every file is taken without asking.
    bool picture = in->file.picture && !pictureFolder_.empty() && a->size <= pictureMax_;
    if (in->file.state == State::Heard && (picture || !autoFolder_.empty()))
    {
        in->file.state = State::Receiving;
        in->file.autoReceived = true;
        const std::string& folder = picture ? pictureFolder_ : autoFolder_;
        in->file.path = files::utf8FromPath(files::pathFromUtf8(folder) / files::pathFromUtf8(in->file.name));
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
    bool parity = in->announced && p->index >= in->file.pieces;
    if (parity)
    {
        if (p->bytes.size() != files::parity::symbolLength(in->announce.pieceBytes)) return;
    }
    else if (in->announced && p->bytes.size() != pieceLength(in->announce.size, in->announce.pieceBytes, p->index))
    {
        return;
    }
    in->file.lastMs = nowMs;
    if (in->file.complete) return;
    if (parity)
    {
        if (in->parity.count(p->index) != 0) return;
        in->parityBytes += p->bytes.size();
        in->parity.emplace(p->index, p->bytes);
        in->parityTop = std::max(in->parityTop, p->index);
        if (in->parityFrom != 0 && p->index >= in->parityFrom) in->parityHeard++;
    }
    else
    {
        if (in->pieces.count(p->index) != 0) return;
        in->bytes += p->bytes.size();
        in->pieces.emplace(p->index, p->bytes);
        in->file.held = (uint16_t)in->pieces.size();
    }
    in->came++;
    if (in->file.state == State::Incomplete && !in->file.stopped)
    {
        // Repairs came after all.
        in->file.state = State::Receiving;
        in->file.expiresAtMs = 0;
    }
    in->rate.sample(in->bytes, nowMs);
    changed();
    tryComplete(*in, nowMs);
    limitSpool();
}

void BroadcastFiles::clearPieces(Incoming& in)
{
    in.pieces.clear();
    in.bytes = 0;
    in.parity.clear();
    in.parityBytes = 0;
    in.file.held = 0;
}

void BroadcastFiles::tryRebuild(Incoming& in)
{
    size_t k = in.file.pieces;
    if (in.pieces.size() >= k || in.pieces.size() + in.parity.size() < k) return;
    std::vector<files::parity::Symbol> known, parity;
    std::vector<uint16_t> missing;
    for (const auto& p : in.pieces) known.push_back({p.first, &p.second});
    for (uint16_t i = 0; i < k; i++)
    {
        if (in.pieces.count(i) == 0) missing.push_back(i);
    }
    for (const auto& p : in.parity)
    {
        if (parity.size() == missing.size()) break;
        parity.push_back({p.first, &p.second});
    }
    std::vector<std::vector<uint8_t>> out;
    bool ok = files::parity::rebuild(known, parity, missing, files::parity::symbolLength(in.announce.pieceBytes), out);
    in.parity.clear();
    in.parityBytes = 0;
    if (!ok) return;
    for (size_t i = 0; i < missing.size(); i++)
    {
        // Rebuilt to the parity's length: the last piece is shorter.
        out[i].resize(pieceLength(in.announce.size, in.announce.pieceBytes, missing[i]));
        in.bytes += out[i].size();
        in.pieces.emplace(missing[i], std::move(out[i]));
    }
    in.file.held = (uint16_t)in.pieces.size();
}

void BroadcastFiles::tryComplete(Incoming& in, uint64_t nowMs)
{
    if (!in.announced || in.file.complete) return;
    tryRebuild(in);
    if (in.pieces.size() < in.file.pieces) return;
    std::vector<uint8_t> content;
    content.reserve(in.announce.size);
    for (const auto& p : in.pieces) content.insert(content.end(), p.second.begin(), p.second.end());
    if (content.size() != in.announce.size || protocol::digestOf(content) != in.announce.digest)
    {
        // Something in it is wrong: all of it again, once.
        clearPieces(in);
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
    dropPart(in);
    in.file.state = state;
    in.file.error = error;
    in.file.lastMs = nowMs;
    clearPieces(in);
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
        f.picture = in.file.picture;
        using Result = files::FinishedFile::Result;
        switch (state)
        {
            case State::Saved:
                f.result = Result::Saved;
                f.path = in.file.path;
                // First heard to saved, repairs and all.
                if (nowMs > in.file.startedMs)
                {
                    f.movedBytes = in.file.size;
                    f.movedMs = nowMs - in.file.startedMs;
                }
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
    in->file.stopped = false;
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
    // Asking starts afresh under its seal, though earlier requests
    // brought nothing.
    if (!seals_.askAgain(in->file.from, in->file.seq, nowMs)) seals_.askSoon(in->file.from, nowMs);
    return true;
}

bool BroadcastFiles::stop(uint64_t id, uint64_t nowMs)
{
    Incoming* in = findIncoming(id);
    if (in == nullptr || in->file.state != State::Receiving) return false;
    in->refusedOut = false;
    in->file.state = State::Incomplete;
    in->file.stopped = true;
    in->file.expiresAtMs = nowMs + PART_KEEP_MS;
    changed();
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
            if (in.file.state != State::Heard || (in.pieces.empty() && in.parity.empty())) continue;
            total += in.bytes + in.parityBytes;
            if (oldest == nullptr || in.file.lastMs < oldest->file.lastMs) oldest = &in;
        }
        if (total <= SPOOL_BYTES || oldest == nullptr) return;
        clearPieces(*oldest);
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
                out.burst = link_.broadcast(out.frames, out.file.mode, nowMs, fileLabel(out.file, out.burstPieces),
                                            true);
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
            // A round carrying its pieces: the screen says to whom.
            bool inRound = seals_.fileRepairs(out.file.seq).roundPieces > 0;
            if (inRound && !out.inRound) emit(Event::Type::Repairing, out.file.id);
            out.inRound = inRound;
            if (out.file.sealed && seals_.repairCapReached(out.file.seq))
            {
                // Its repairs have had the airtime the operator allows.
                seals_.closeNow(out.file.seq);
                out.capped = true;
                finishOutgoing(out, out.file.stopped ? State::Cancelled : State::Sent, nowMs);
            }
            else if (out.file.sealed && !seal.open)
            {
                finishOutgoing(out, out.file.stopped ? State::Cancelled : State::Sent, nowMs);
            }
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
        bool sealed = in.announced && seals_.sealHeard(in.file.from, in.file.seq);
        if (sealed) in.sealSeen = true;
        // Repairs are over for it: we gave up asking (requests brought
        // nothing), the sender refused, its seal has aged out, or nothing
        // has come for long.
        bool refused = seals_.refusal(in.file.from, in.file.seq).has_value();
        if (in.file.state == State::Receiving && !in.file.complete && in.announced
            && ((sealed && seals_.gaveUp(in.file.from, in.file.seq)) || refused || (in.sealSeen && !sealed)
                || nowMs >= in.file.lastMs + INCOMPLETE_MS))
        {
            in.file.state = State::Incomplete;
            in.file.expiresAtMs = nowMs + PART_KEEP_MS;
            in.refusedOut = refused;
            changed();
        }
        else if (in.file.state == State::Incomplete && in.refusedOut && !refused && sealed)
        {
            // The sender re-opened its repairs (its seal came again after
            // the refusal): we were receiving it, so we ask again.
            in.refusedOut = false;
            in.file.state = State::Receiving;
            in.file.expiresAtMs = 0;
            in.file.lastMs = nowMs;
            if (!seals_.askAgain(in.file.from, in.file.seq, nowMs)) seals_.askSoon(in.file.from, nowMs);
            emit(Event::Type::Reopened, in.file.id);
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
    saveParts(nowMs);
}

//-------------------------------------------------------------------------
// Parts kept on disk
//-------------------------------------------------------------------------

namespace
{

// A part's file name: the sender, the announce's number and the file's
// digest, in characters any file system takes.
std::string partName(const std::string& from, uint32_t seq, const protocol::Digest& digest)
{
    std::string name;
    for (char c : from) name += std::isalnum((unsigned char)c) ? c : '_';
    static const char* const digits = "0123456789abcdef";
    name += "-" + std::to_string(seq) + "-";
    for (size_t i = 0; i < 4 && i < digest.size(); i++)
    {
        name += digits[digest[i] >> 4];
        name += digits[digest[i] & 0x0f];
    }
    return name + ".part";
}

bool keptState(BroadcastFile::State s)
{
    return s == BroadcastFile::State::Heard || s == BroadcastFile::State::Receiving
           || s == BroadcastFile::State::Incomplete;
}

} // namespace

void BroadcastFiles::dropPart(Incoming& in)
{
    if (in.partFile.empty()) return;
    std::error_code ec;
    std::filesystem::remove(files::pathFromUtf8(in.partFile), ec);
    in.partFile.clear();
    in.savedSig.clear();
}

void BroadcastFiles::saveParts(uint64_t nowMs)
{
    if (partsSavedMs_ != 0 && nowMs < partsSavedMs_ + PART_SAVE_MS) return;
    partsSavedMs_ = nowMs;
    writeParts();
}

void BroadcastFiles::writeParts()
{
    if (partsFolder_.empty()) return;
    std::filesystem::path folder = files::pathFromUtf8(partsFolder_);
    for (Incoming& in : incoming_)
    {
        // Saved, ignored and the like were dropped as they finished; a
        // spool cleared of its pieces has nothing left to keep.
        if (!in.announced || !keptState(in.file.state) || (in.pieces.empty() && in.parity.empty()))
        {
            dropPart(in);
            continue;
        }
        std::vector<KeptSeal> seals = seals_.sealsOver(in.file.from, in.file.seq);
        std::string sig = std::to_string((int)in.file.state) + " " + std::to_string(in.pieces.size()) + "+"
                          + std::to_string(in.parity.size()) + " "
                          + std::to_string(in.file.stopped) + " " + std::to_string(in.file.expiresAtMs) + " "
                          + in.file.path + " " + std::to_string(seals.size());
        for (const KeptSeal& s : seals) sig += " " + std::to_string(s.heardMs);
        if (sig == in.savedSig) continue;

        BroadcastPart part;
        part.announce = in.announceRecord;
        part.state = (int)in.file.state;
        part.stopped = in.file.stopped;
        part.autoReceived = in.file.autoReceived;
        part.chosenPath = in.chosenPath;
        part.path = in.file.path;
        part.startedMs = in.file.startedMs;
        part.lastMs = in.file.lastMs;
        part.expiresAtMs = in.file.state == State::Heard ? in.file.startedMs + KEEP_MS : in.file.expiresAtMs;
        part.seals = std::move(seals);
        // Parity pieces with the rest, by their indexes past the piece count.
        part.pieces = in.pieces;
        part.pieces.insert(in.parity.begin(), in.parity.end());

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::filesystem::path target = folder / partName(in.file.from, in.file.seq, in.announce.digest);
        std::filesystem::path temp = target;
        temp += ".new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << encodePart(part);
            out.close();
            if (!out) continue;
        }
        std::filesystem::rename(temp, target, ec);
        if (ec) continue;
        in.partFile = files::utf8FromPath(target);
        in.savedSig = sig;
    }
}

void BroadcastFiles::setPartsFolder(const std::string& folder, uint64_t nowMs)
{
    partsFolder_ = folder;
    partsSavedMs_ = 0;
    if (!folder.empty()) loadParts(nowMs);
}

void BroadcastFiles::loadParts(uint64_t nowMs)
{
    std::error_code ec;
    std::filesystem::path folder = files::pathFromUtf8(partsFolder_);
    // By name, so the same folder reads back the same way everywhere (a
    // directory's own order differs from one file system to the next).
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        if (entry.path().extension() == ".part") found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    for (const std::filesystem::path& path : found)
    {
        std::string file = files::utf8FromPath(path);
        std::string text;
        {
            // Closed before anything is deleted: Windows won't delete an open file.
            std::ifstream in(path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        BroadcastPart part;
        bool good = decodePart(text, part);
        const protocol::FileAnnounce* a = good ? part.announce.as<protocol::FileAnnounce>() : nullptr;
        State state = (State)part.state;
        // A second copy of a part already read back (copied by hand, say)
        // is left alone; one that doesn't parse is deleted whatever it names.
        bool known = false;
        for (const Incoming& other : incoming_)
        {
            if (a != nullptr && other.file.from == part.announce.header.callsign
                && other.file.seq == part.announce.header.seq)
                known = true;
        }
        if (a == nullptr || known || !keptState(state) || a->size > MAX_FILE_BYTES || a->pieceBytes == 0
            || (part.expiresAtMs != 0 && nowMs >= part.expiresAtMs))
        {
            if (!known) std::filesystem::remove(path, ec);
            continue;
        }

        incoming_.emplace_back();
        Incoming& kept = incoming_.back();
        kept.file.id = nextId_++;
        kept.file.outgoing = false;
        kept.file.from = part.announce.header.callsign;
        kept.file.seq = part.announce.header.seq;
        kept.announced = true;
        kept.announce = *a;
        kept.announceRecord = part.announce;
        kept.file.name = files::safeFileName(a->name);
        kept.file.size = a->size;
        kept.file.picture = protocol::isPictureKind(a->kind);
        uint16_t pieces = (uint16_t)((a->size + a->pieceBytes - 1) / a->pieceBytes);
        kept.file.pieces = pieces;
        kept.file.pieceMap.assign(pieces, files::PIECE_WAITING);
        // Nothing read back is trusted: a piece must fit the announce.
        for (auto& p : part.pieces)
        {
            if (p.first >= pieces && p.second.size() == files::parity::symbolLength(a->pieceBytes))
            {
                kept.parityBytes += p.second.size();
                kept.parity.emplace(p.first, std::move(p.second));
                continue;
            }
            if (p.first >= pieces || p.second.size() != pieceLength(a->size, a->pieceBytes, p.first)) continue;
            kept.bytes += p.second.size();
            kept.pieces.emplace(p.first, std::move(p.second));
        }
        kept.file.held = (uint16_t)kept.pieces.size();
        kept.file.state = state;
        kept.file.stopped = part.stopped;
        kept.file.autoReceived = part.autoReceived;
        kept.chosenPath = part.chosenPath;
        kept.file.path = part.path;
        kept.file.startedMs = part.startedMs;
        kept.file.lastMs = part.lastMs;
        kept.file.expiresAtMs = state == State::Incomplete ? part.expiresAtMs : 0;
        kept.sealSeen = !part.seals.empty();
        kept.partFile = file;
        if (state == State::Receiving && kept.file.path.empty()) kept.file.state = State::Incomplete;
        seals_.restoreHeard(part.announce, part.seals, part.lastMs, nowMs);
        changed();
        tryComplete(kept, nowMs);
    }
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
    f.repairGear = seals_.repairGear(f.seq);
    f.repairSeconds = seals_.repairAirtime(f.seq);
    f.repairCapped = out.capped;
    if (f.state == State::Repairing)
    {
        Seals::SealView seal = seals_.sealOf(f.seq, nowMs);
        f.sealed = seal.sealed;
        f.repairsUntilMs = seal.openUntilMs;
        f.keptOpen = seal.keptOpen;
        f.askers = seal.askers;
    }
    else if ((f.state == State::Sent || f.state == State::Cancelled) && !f.path.empty())
    {
        f.reopenable = seals_.sealOf(f.seq, nowMs).sealed;
    }
    if (f.state == State::Repairing || f.state == State::Sent || f.state == State::Cancelled)
    {
        Seals::FileRepairs repairs = seals_.fileRepairs(f.seq);
        f.askedBy = std::move(repairs.askers);
        f.repairPieces = repairs.piecesSent;
        if (f.state == State::Repairing)
        {
            f.repairingFor = std::move(repairs.sendingTo);
            f.roundPieces = repairs.roundPieces;
            f.roundSent = repairs.roundSent;
        }
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
    f.sealAged = in.sealSeen && !f.sealed;
    f.refusal = seals_.refusal(f.from, f.seq);
    if (in.announced)
    {
        // Missed: its turn has gone by (a later piece is here, or the seal).
        uint16_t last = in.pieces.empty() ? 0 : in.pieces.rbegin()->first;
        size_t lacking = in.pieces.size() < f.pieces ? (size_t)f.pieces - in.pieces.size() : 0;
        f.repairHeld = (uint16_t)std::min(in.parity.size(), lacking);
        // The repair pieces held stand in for missing pieces, any of them:
        // shown over the first gaps.
        uint16_t covered = f.repairHeld;
        for (uint16_t i = 0; i < f.pieces; i++)
        {
            if (in.pieces.count(i) != 0)
                f.pieceMap[i] = files::PIECE_HELD;
            else if (covered > 0 && covered--)
                f.pieceMap[i] = files::PIECE_COVERED;
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
            // Who asked, and a round as it goes (Jeff, 2026-10-26: one of 39
            // repair pieces went out with nothing on the screen saying so).
            std::string repairs = repairText(f);
            if (!f.sealed)
                row.state = "Sealing";
            else if (f.roundPieces > 0)
                row.state = "Sending repairs" + repairs.substr(std::string("sending repairs").size());
            else
                row.state = "Repairing, " + repairs;
            if (f.stopped)
                row.state = "Stopped at " + std::to_string(f.held) + " of " + std::to_string(f.pieces) + ", "
                            + (!f.sealed ? std::string("sealing")
                               : f.askedBy.empty() && f.askers == 0 && f.repairPieces == 0 ? std::string("repairs open")
                                                                                            : repairs);
            row.look = f.roundPieces > 0 ? Look::Moving : Look::Repairing;
            row.expiresAtMs = f.repairsUntilMs;
        }
        rows.push_back(std::move(row));
    }
    for (const Incoming& in : incoming_)
    {
        if (!in.announced || !in.file.live()) continue;
        BroadcastFile f = view(in, nowMs);
        files::FileRow row = base(f);
        // Repair pieces held count toward the whole: they make up the
        // missing pieces once there are enough of them (KC2G, 2026-10-26:
        // "my completion doesn't go up at all").
        row.done = std::min<uint64_t>(f.size, in.bytes + (uint64_t)f.repairHeld * in.announce.pieceBytes);
        row.handed = row.done;
        size_t have = in.pieces.size() + in.parity.size();
        size_t missing = have < f.pieces ? (size_t)f.pieces - have : 0;
        if (f.state == State::Heard)
        {
            row.state = "Heard: Receive or Reject";
            row.look = Look::Waiting;
            row.expiresAtMs = f.expiresAtMs;
        }
        else if (f.state == State::Incomplete)
        {
            row.state = std::string(f.stopped ? "Stopped, " : "Part kept, ") + std::to_string(in.pieces.size()) + " of "
                        + countText(f.pieces, "piece", "pieces");
            if (f.refusal)
                row.state = "Not repaired: " + f.from + " " + refusalText(*f.refusal) + ", "
                            + std::to_string(in.pieces.size()) + " of " + countText(f.pieces, "piece", "pieces")
                            + " kept";
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
