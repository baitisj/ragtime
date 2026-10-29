// Offer, answer, data, saved, cancel and expiry for files in a session.
// Ported from Glissando's Data2GFileTransfer; see the header.
#include "ragtime/files/SessionFiles.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include "ragtime/files/Sha256.h"

namespace ragtime
{
namespace files
{

namespace
{

using protocol::CancelReason;

// Finished transfers kept for the screen to show, past which the oldest go.
constexpr size_t FINISHED_KEPT = 64;

const char* const FALLBACK_NAME = "received-file";

const char* const NOT_ALLOWED = "Sending is not allowed here.";

// The length of the UTF-8 sequence starting at text[at], or 0 when it is
// not a whole, valid one.
size_t utf8Length(const std::string& text, size_t at)
{
    unsigned char c = (unsigned char)text[at];
    size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (length == 0 || (length == 2 && c < 0xC2) || at + length > text.size()) return 0;
    for (size_t i = 1; i < length; i++)
    {
        if (((unsigned char)text[at + i] & 0xC0) != 0x80) return 0;
    }
    return length;
}

// At most bytes long, cut where a character ends.
std::string truncateUtf8(const std::string& text, size_t bytes)
{
    if (text.size() <= bytes) return text;
    size_t at = 0;
    while (at < text.size())
    {
        size_t length = utf8Length(text, at);
        if (length == 0) length = 1;
        if (at + length > bytes) break;
        at += length;
    }
    return text.substr(0, at);
}

bool reservedOnWindows(const std::string& stem)
{
    std::string upper;
    for (char c : stem) upper.push_back((char)std::toupper((unsigned char)c));
    while (!upper.empty() && upper.back() == ' ') upper.pop_back();
    static const char* const devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (const char* device : devices)
    {
        if (upper == device) return true;
    }
    bool port = upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0;
    if (!port) return false;
    if (upper.size() == 4) return upper[3] >= '0' && upper[3] <= '9';
    // Superscript one, two and three name the same ports.
    return upper.size() == 5 && (unsigned char)upper[3] == 0xC2 &&
           ((unsigned char)upper[4] == 0xB9 || (unsigned char)upper[4] == 0xB2 || (unsigned char)upper[4] == 0xB3);
}

uint32_t unixSeconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

} // namespace

bool FileTransfer::live() const
{
    switch (state)
    {
        case State::Waiting:
        case State::Offered:
        case State::Sending:
        case State::Asking:
        case State::Receiving:
            return true;
        default:
            return false;
    }
}

std::string safeFileName(const std::string& offered)
{
    // Only what follows the last separator of either kind.
    size_t slash = offered.find_last_of("/\\");
    std::string last = slash == std::string::npos ? offered : offered.substr(slash + 1);

    std::string clean;
    for (size_t at = 0; at < last.size();)
    {
        size_t length = utf8Length(last, at);
        if (length == 0)
        {
            at++; // not UTF-8: dropped
            continue;
        }
        unsigned char c = (unsigned char)last[at];
        bool control = c < 0x20 || c == 0x7F ||
                       (length == 2 && c == 0xC2 && (unsigned char)last[at + 1] < 0xA0); // C1 controls
        bool refused = length == 1 && std::string("<>:\"|?*").find((char)c) != std::string::npos;
        // Direction marks, embeddings, overrides and isolates, which would
        // let a name show another extension than it has.
        if (length == 3 && c == 0xE2)
        {
            unsigned char c1 = (unsigned char)last[at + 1], c2 = (unsigned char)last[at + 2];
            bool marks = c1 == 0x80 && (c2 == 0x8E || c2 == 0x8F || (c2 >= 0xAA && c2 <= 0xAE)); // U+200E-F, U+202A-E
            bool isolates = c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9;                             // U+2066-9
            refused = refused || marks || isolates;
        }
        if (!control && !refused) clean.append(last, at, length);
        at += length;
    }

    // No hidden files, and nothing Windows would trim or refuse.
    size_t first = clean.find_first_not_of(". ");
    clean = first == std::string::npos ? std::string() : clean.substr(first);
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();

    clean = truncateUtf8(clean, protocol::MAX_FILE_NAME);
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
    if (clean.empty()) return FALLBACK_NAME;

    size_t dot = clean.find('.');
    if (reservedOnWindows(clean.substr(0, dot)))
    {
        return std::string(FALLBACK_NAME) + (dot == std::string::npos ? std::string() : clean.substr(dot));
    }
    return clean;
}

std::filesystem::path pathFromUtf8(const std::string& utf8)
{
    return std::filesystem::u8path(utf8);
}

std::string utf8FromPath(const std::filesystem::path& path)
{
    return path.u8string();
}

std::filesystem::path freeSavePath(const std::filesystem::path& folder, const std::string& name)
{
    std::filesystem::path wanted = folder / pathFromUtf8(name);
    auto taken = [](const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::path part = path;
        part += ".part";
        return std::filesystem::exists(path, ec) || std::filesystem::exists(part, ec);
    };
    if (!taken(wanted)) return wanted;

    std::filesystem::path stem = wanted.stem();
    std::filesystem::path extension = wanted.extension();
    for (int n = 2; n < 1000; n++)
    {
        std::filesystem::path candidate = folder / stem;
        candidate += " (" + std::to_string(n) + ")";
        candidate += extension;
        if (!taken(candidate)) return candidate;
    }
    return std::filesystem::path();
}

bool fileDigest(const std::filesystem::path& path, uint64_t size, protocol::Digest& digest)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Sha256 hash;
    std::vector<uint8_t> buffer(65536);
    uint64_t left = size;
    while (left > 0)
    {
        size_t want = (size_t)std::min<uint64_t>(buffer.size(), left);
        in.read((char*)buffer.data(), (std::streamsize)want);
        if ((size_t)in.gcount() != want) return false;
        hash.update(buffer.data(), want);
        left -= want;
    }
    Sha256::Digest full = hash.finish();
    std::copy(full.begin(), full.begin() + protocol::DIGEST_BYTES, digest.begin());
    return true;
}

SessionFiles::~SessionFiles()
{
    // What was under way is kept as a part, and parts stay on disk: the
    // owner saved keptParts() first, to carry on after a restart.
    stopAll("closed");
}

SessionFiles::Entry* SessionFiles::find(uint64_t id)
{
    for (Entry& entry : entries_)
    {
        if (entry.transfer.id == id) return &entry;
    }
    return nullptr;
}

SessionFiles::Entry* SessionFiles::findOutgoing(const std::string& peer, uint32_t offer)
{
    for (Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        bool offered = t.live() && t.state != FileTransfer::State::Waiting;
        if (t.outgoing && t.peer == peer && entry.offer == offer && offered) return &entry;
    }
    return nullptr;
}

SessionFiles::Entry* SessionFiles::findIncoming(const std::string& peer, uint32_t offer)
{
    for (Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        if (!t.outgoing && t.peer == peer && entry.offer == offer && t.live()) return &entry;
    }
    return nullptr;
}

void SessionFiles::queue(const std::string& peer, protocol::Body body)
{
    pending_[peer].push_back(std::move(body));
}

void SessionFiles::queueCancel(Entry& entry, CancelReason reason)
{
    protocol::FileCancel cancel;
    cancel.offer = entry.offer;
    cancel.reason = (uint8_t)reason;
    queue(entry.transfer.peer, cancel);
}

// The transfer is over: files are closed, and a part file of one not
// saved is kept for the same file sent again, or deleted. One of ours
// that some of went out is kept as a part sent.
void SessionFiles::finish(Entry& entry, FileTransfer::State state, const std::string& error)
{
    FileTransfer& t = entry.transfer;
    t.state = state;
    if (!error.empty()) t.error = error;
    if (entry.in.is_open()) entry.in.close();
    if (entry.out.is_open())
    {
        // What has come of a piece still arriving is as good as the rest:
        // the session's stream comes in order, and the digest checks the
        // whole file in the end.
        size_t more = entry.arrivingData.size();
        if (state != FileTransfer::State::Saved && !entry.spoiled && more > 0 && entry.arrivingAt == t.done
            && t.done + more < t.size)
        {
            entry.out.write((const char*)entry.arrivingData.data(), (std::streamsize)more);
            entry.out.flush();
            if (entry.out) t.done += more;
            else entry.spoiled = true;
        }
        entry.out.close();
        if (state != FileTransfer::State::Saved) keep(entry);
    }
    if (t.outgoing) keepSent(entry);
    entry.part.clear();
    entry.pieces.clear();
    entry.arriving = 0;
    entry.arrivingData.clear();
    changed();
}

// Called only where a transfer is added, never while the list is being
// walked.
void SessionFiles::prune()
{
    size_t finished = (size_t)std::count_if(entries_.begin(), entries_.end(),
                                            [](const Entry& e) { return !e.transfer.live(); });
    for (auto it = entries_.begin(); it != entries_.end() && finished > FINISHED_KEPT;)
    {
        if (it->transfer.live())
        {
            ++it;
            continue;
        }
        it = entries_.erase(it);
        finished--;
    }
}

uint64_t SessionFiles::offer(const std::string& peer, const std::string& path, uint64_t nowMs, std::string& error,
                            bool picture)
{
    if (peer.empty())
    {
        error = "No station to send the file to.";
        return 0;
    }
    if (inhibited_)
    {
        error = NOT_ALLOWED;
        return 0;
    }

    std::filesystem::path file = pathFromUtf8(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
    {
        error = "\"" + path + "\" is not a file that can be sent.";
        return 0;
    }
    uint64_t size = std::filesystem::file_size(file, ec);
    if (ec)
    {
        error = "\"" + path + "\" could not be read: " + ec.message();
        return 0;
    }
    if (size > MAX_FILE_BYTES)
    {
        error = "\"" + path + "\" is larger than the 300 KB Ragtime sends over HF.";
        return 0;
    }
    protocol::Digest digest{};
    if (!fileDigest(file, size, digest))
    {
        error = "\"" + path + "\" could not be read.";
        return 0;
    }

    Entry& entry = entries_.emplace_back();
    FileTransfer& t = entry.transfer;
    t.id = nextId_++;
    t.outgoing = true;
    t.peer = peer;
    t.name = truncateUtf8(utf8FromPath(file.filename()), protocol::MAX_FILE_NAME);
    if (t.name.empty()) t.name = "file";
    t.size = size;
    t.path = path;
    t.state = FileTransfer::State::Waiting;
    t.digest = digest;
    t.atMs = nowMs;
    t.picture = picture;
    entry.kind = protocol::FILE_PLAIN;
    if (picture)
    {
        // The codec, from the picture's first bytes.
        uint8_t head[4] = {};
        std::ifstream first(file, std::ios::binary);
        first.read(reinterpret_cast<char*>(head), sizeof head);
        entry.kind = protocol::pictureKindOf(head, (size_t)first.gcount());
    }
    entry.sinceMs = nowMs;
    // Sent again: the far end carries on from its part, and this transfer
    // stands for the part sent.
    auto sent = findSent(entry);
    if (sent != kept_.end()) dropKept(sent, false);
    changed();
    uint64_t id = t.id;
    prune();
    return id;
}

// "<path>.part", or "<path>.2.part" and so on when a file of that name is
// there already or another transfer is writing it; empty if none is free.
std::filesystem::path SessionFiles::freePart(const std::string& path) const
{
    for (int n = 1; n < 1000; n++)
    {
        std::filesystem::path part = pathFromUtf8(path);
        if (n > 1) part += "." + std::to_string(n);
        part += ".part";
        std::error_code ec;
        bool taken = std::filesystem::exists(part, ec) ||
                     std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.part == part; });
        if (!taken) return part;
    }
    return std::filesystem::path();
}

// A part file kept for the same file from the same station, if any.
std::list<SessionFiles::Kept>::iterator SessionFiles::findKept(const Entry& entry)
{
    const FileTransfer& t = entry.transfer;
    return std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) {
        return !k.outgoing && k.peer == t.peer && k.size == t.size && k.digest == t.digest;
    });
}

// A part sent of the same file to the same station, if any.
std::list<SessionFiles::Kept>::iterator SessionFiles::findSent(const Entry& entry)
{
    const FileTransfer& t = entry.transfer;
    return std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) {
        return k.outgoing && k.peer == t.peer && k.size == t.size && k.digest == t.digest;
    });
}

size_t SessionFiles::keptCount() const
{
    return (size_t)std::count_if(kept_.begin(), kept_.end(), [](const Kept& k) { return !k.outgoing; });
}

// Keeps what has come of a transfer of theirs that ended unfinished, if
// there is anything and it can be trusted; otherwise its part file goes.
void SessionFiles::keep(Entry& entry)
{
    const FileTransfer& t = entry.transfer;
    std::error_code ec;
    if (entry.spoiled || t.done == 0 || t.done >= t.size)
    {
        std::filesystem::remove(entry.part, ec);
        return;
    }
    auto older = findKept(entry);
    if (older != kept_.end()) dropKept(older, false);
    Kept& k = kept_.emplace_back();
    k.id = nextId_++;
    k.transferId = t.id;
    entry.transfer.partKept = true;
    k.peer = t.peer;
    k.name = t.name;
    k.size = t.size;
    k.digest = t.digest;
    k.path = t.path;
    k.part = entry.part;
    k.bytes = t.done;
    k.keptAtMs = nowMs_;
    limitKept(nowMs_);
    changed();
}

// Ours, ended unfinished with some of it written into the session: the
// far end keeps what came as its part, so the file isn't finished here
// either. The host sends what it has been handed, cancelled or not, and
// it is counted as it is acknowledged; a far end that has stopped takes
// no more.
void SessionFiles::keepSent(Entry& entry)
{
    using State = FileTransfer::State;
    FileTransfer& t = entry.transfer;
    bool unfinished = t.state == State::Cancelled || t.state == State::CancelledThere || t.state == State::Failed;
    if (!unfinished || entry.written == 0) return;
    if (t.state != State::Failed && entry.written > t.done) cutsWanted_++;
    auto older = findSent(entry);
    if (older != kept_.end()) dropKept(older, false);
    Kept& k = kept_.emplace_back();
    k.id = nextId_++;
    k.outgoing = true;
    k.transferId = t.id;
    t.partKept = true;
    k.peer = t.peer;
    k.name = t.name;
    k.size = t.size;
    k.digest = t.digest;
    k.path = t.path;
    k.bytes = t.done;
    k.handed = k.bytes;
    k.offer = entry.offer;
    if (t.state != State::CancelledThere)
    {
        k.handed = std::max(k.bytes, entry.written);
        k.pieces = entry.pieces;
    }
    k.keptAtMs = nowMs_;
    limitKept(nowMs_);
    changed();
}

void SessionFiles::dropKept(std::list<Kept>::iterator it, bool ended)
{
    if (ended && it->hasEnding) ended_.push_back(it->ending);
    std::error_code ec;
    if (!it->part.empty()) std::filesystem::remove(it->part, ec);
    kept_.erase(it);
    changed();
}

// A part sent that is done with now: delivered after all, or with none of
// it acknowledged when the session ended. Its transfer's ending goes to
// Completed (as Sent, delivered); one not yet taken there goes by itself.
void SessionFiles::endSent(std::list<Kept>::iterator it, bool delivered)
{
    if (it->hasEnding)
    {
        FinishedFile f = it->ending;
        if (delivered)
        {
            f.result = FinishedFile::Result::Sent;
            f.reason.clear();
        }
        ended_.push_back(std::move(f));
    }
    else
    {
        for (Entry& e : entries_)
        {
            if (e.transfer.id == it->transferId) e.transfer.partKept = false;
        }
    }
    kept_.erase(it);
    changed();
}

// The session with the station is over (or every one is, for an empty
// peer): what the host still had of a part sent won't go now.
void SessionFiles::stopSent(const std::string& peer)
{
    for (auto it = kept_.begin(); it != kept_.end();)
    {
        auto next = std::next(it);
        if (it->outgoing && (peer.empty() || it->peer == peer) && !it->pieces.empty())
        {
            it->pieces.clear();
            it->handed = it->bytes;
            changed();
            if (it->bytes == 0) endSent(it, false);
        }
        it = next;
    }
}

void SessionFiles::limitKept(uint64_t nowMs)
{
    for (auto it = kept_.begin(); it != kept_.end();)
    {
        // Kept where no time was to hand: its time starts now.
        if (it->keptAtMs == 0) it->keptAtMs = nowMs;
        if (it->keepUntilMs == 0) it->keepUntilMs = it->keptAtMs + KEPT_MS;
        bool old = nowMs >= it->keepUntilMs;
        auto next = std::next(it);
        if (old) dropKept(it, true);
        it = next;
    }
    // So many each way at most, the oldest going first.
    for (bool outgoing : {false, true})
    {
        size_t n = (size_t)std::count_if(kept_.begin(), kept_.end(),
                                         [outgoing](const Kept& k) { return k.outgoing == outgoing; });
        for (auto it = kept_.begin(); it != kept_.end() && n > KEPT_MAX;)
        {
            auto next = std::next(it);
            if (it->outgoing == outgoing)
            {
                dropKept(it, true);
                n--;
            }
            it = next;
        }
    }
}

// Carries on in the part file kept for the offer, with the rest asked for.
// False, with nothing changed, if it is gone or not as it was left.
bool SessionFiles::resumePart(Entry& entry, const std::string& path)
{
    FileTransfer& t = entry.transfer;
    auto kept = findKept(entry);
    if (kept == kept_.end() || kept->path != path) return false;
    std::error_code ec;
    if (std::filesystem::file_size(kept->part, ec) != kept->bytes || ec)
    {
        dropKept(kept, false);
        return false;
    }
    entry.out.open(kept->part, std::ios::binary | std::ios::app);
    if (!entry.out)
    {
        dropKept(kept, false);
        return false;
    }
    entry.part = kept->part;
    t.path = path;
    t.done = kept->bytes;
    t.resumedFrom = kept->bytes;
    t.kept = 0;
    t.keptPath.clear();
    t.state = FileTransfer::State::Receiving;
    kept_.erase(kept);

    protocol::FileAnswer answer;
    answer.offer = entry.offer;
    answer.accept = true;
    answer.from = (uint32_t)t.done;
    queue(t.peer, answer);
    changed();

    // All of it was here already.
    if (t.done == t.size) completeReceive(entry);
    return true;
}

bool SessionFiles::openPart(Entry& entry, const std::string& path, std::string& error)
{
    FileTransfer& t = entry.transfer;
    std::filesystem::path part = freePart(path);
    if (part.empty())
    {
        error = "No part file could be made beside \"" + path + "\".";
        return false;
    }
    entry.out.open(part, std::ios::binary | std::ios::trunc);
    if (!entry.out)
    {
        error = "\"" + utf8FromPath(part) + "\" could not be written.";
        return false;
    }
    entry.part = part;
    t.path = path;
    t.done = 0;
    t.kept = 0;
    t.keptPath.clear();
    t.state = FileTransfer::State::Receiving;
    protocol::FileAnswer answer;
    answer.offer = entry.offer;
    answer.accept = true;
    queue(t.peer, answer);
    changed();

    // Nothing is coming for an empty file: it is saved now.
    if (t.size == 0) completeReceive(entry);
    return true;
}

bool SessionFiles::accept(uint64_t id, const std::string& path, std::string& error)
{
    Entry* entry = find(id);
    if (entry == nullptr || entry->transfer.outgoing || entry->transfer.state != FileTransfer::State::Asking)
    {
        error = entry != nullptr && entry->transfer.state == FileTransfer::State::Expired
                    ? "The offer has expired."
                    : "The offer is no longer open.";
        return false;
    }
    if (inhibited_)
    {
        error = NOT_ALLOWED;
        return false;
    }
    for (const Entry& other : entries_)
    {
        const FileTransfer& o = other.transfer;
        if (&other != entry && !o.outgoing && o.state == FileTransfer::State::Receiving && o.path == path)
        {
            // Left open, for another name.
            error = "Another file is being saved as \"" + path + "\".";
            return false;
        }
    }
    if (resumePart(*entry, path)) return true;
    // Saved somewhere else: what was kept of it is of no more use.
    auto kept = findKept(*entry);
    if (kept != kept_.end()) dropKept(kept, false);
    if (openPart(*entry, path, error)) return true;

    // The far end hears that this side can't take it.
    entry->transfer.path = path;
    queueCancel(*entry, CancelReason::ReceiverFailed);
    finish(*entry, FileTransfer::State::Failed, error);
    return false;
}

bool SessionFiles::decline(uint64_t id)
{
    Entry* entry = find(id);
    if (entry == nullptr || entry->transfer.outgoing || entry->transfer.state != FileTransfer::State::Asking)
    {
        return false;
    }
    protocol::FileAnswer answer;
    answer.offer = entry->offer;
    answer.accept = false;
    queue(entry->transfer.peer, answer);
    finish(*entry, FileTransfer::State::Declined);
    return true;
}

bool SessionFiles::cancel(uint64_t id)
{
    Entry* entry = find(id);
    if (entry == nullptr || !entry->transfer.live()) return false;

    using State = FileTransfer::State;
    switch (entry->transfer.state)
    {
        case State::Waiting:
            // Never offered: nothing to tell the far end.
            finish(*entry, State::Cancelled);
            break;
        case State::Offered:
        case State::Sending:
            // With all of it written, the far end may have saved it
            // already, and then ignores the cancel: its FileSaved still
            // counts.
            entry->awaitingSaved = entry->transfer.state == State::Sending && entry->written >= entry->transfer.size;
            queueCancel(*entry, CancelReason::SenderStopped);
            finish(*entry, State::Cancelled);
            break;
        case State::Asking:
            return decline(id);
        case State::Receiving:
            queueCancel(*entry, CancelReason::ReceiverStopped);
            finish(*entry, State::Cancelled);
            break;
        default:
            return false;
    }
    return true;
}

void SessionFiles::setAutoPictures(const std::string& folder, uint64_t maxBytes)
{
    pictureFolder_ = folder;
    pictureMax_ = maxBytes;
}

bool SessionFiles::enough(uint64_t id)
{
    Entry* entry = find(id);
    if (entry == nullptr || entry->transfer.outgoing || !entry->transfer.picture
        || entry->transfer.state != FileTransfer::State::Receiving)
    {
        return false;
    }
    FileTransfer& t = entry->transfer;
    // What has come of a piece still arriving counts: the stream comes in
    // order.
    size_t more = entry->arrivingData.size();
    bool arriving = !entry->spoiled && more > 0 && entry->arrivingAt == t.done && t.done + more < t.size;
    if (entry->spoiled || (t.done == 0 && !arriving)) return cancel(id);
    if (arriving)
    {
        entry->out.write((const char*)entry->arrivingData.data(), (std::streamsize)more);
        if (entry->out) t.done += more;
    }
    entry->out.close();
    if (entry->out.fail())
    {
        std::error_code ec;
        std::filesystem::remove(entry->part, ec);
        queueCancel(*entry, CancelReason::ReceiverFailed);
        finish(*entry, FileTransfer::State::Failed, "\"" + utf8FromPath(entry->part) + "\" could not be written.");
        return true;
    }
    queueCancel(*entry, CancelReason::ReceiverHasEnough);
    if (!placePart(*entry))
    {
        std::error_code ec;
        std::filesystem::remove(entry->part, ec);
        finish(*entry, FileTransfer::State::Failed, "\"" + t.path + "\" could not be saved.");
        return true;
    }
    t.enough = true;
    finish(*entry, FileTransfer::State::Saved);
    return true;
}

bool SessionFiles::pictureBytes(uint64_t id, std::vector<uint8_t>& out) const
{
    out.clear();
    const Entry* entry = nullptr;
    for (const Entry& e : entries_)
        if (e.transfer.id == id) entry = &e;
    if (entry == nullptr) return false;
    const FileTransfer& t = entry->transfer;
    using State = FileTransfer::State;
    bool whole = t.outgoing || t.state == State::Saved;
    std::filesystem::path from = whole ? pathFromUtf8(t.path) : entry->part;
    uint64_t length = whole ? std::min<uint64_t>(t.size, MAX_FILE_BYTES) : t.done;
    if (from.empty() || (!whole && t.state != State::Receiving)) return false;
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    out.resize((size_t)length);
    in.read((char*)out.data(), (std::streamsize)length);
    out.resize((size_t)in.gcount());
    // The bytes of a FileData still coming in follow on.
    if (!whole && entry->arrivingAt == out.size())
        out.insert(out.end(), entry->arrivingData.begin(), entry->arrivingData.end());
    return !out.empty();
}

void SessionFiles::setAutoAccept(const std::string& folder, const std::vector<std::string>& calls)
{
    autoFolder_ = folder;
    autoCalls_.clear();
    for (const std::string& call : calls)
    {
        std::string normal = protocol::normalCallsign(call);
        if (!normal.empty()) autoCalls_.insert(normal);
    }
}

std::vector<FileTransfer> SessionFiles::transfers(uint64_t nowMs) const
{
    std::vector<FileTransfer> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_)
    {
        FileTransfer& t = out.emplace_back(entry.transfer);
        t.handed = t.outgoing ? std::max(entry.written, t.done) : std::min(t.size, t.done + entry.arriving);
        bool open = t.state == FileTransfer::State::Offered || t.state == FileTransfer::State::Asking;
        if (open && nowMs != 0)
        {
            uint64_t age = nowMs > entry.sinceMs ? nowMs - entry.sinceMs : 0;
            t.expiresInMs = age < OFFER_EXPIRY_MS ? OFFER_EXPIRY_MS - age : 0;
        }
    }
    return out;
}

std::vector<uint8_t> SessionFiles::takeRecords(const std::string& peer, protocol::Numbering& numbering,
                                               uint64_t nowMs, bool offerNext)
{
    std::vector<uint8_t> out;
    uint32_t time = unixSeconds(nowMs);
    auto pending = pending_.find(peer);
    if (pending != pending_.end())
    {
        for (protocol::Body& body : pending->second) protocol::encodeRecord(numbering.record(std::move(body), time), out);
        pending_.erase(pending);
    }

    // The next file for the station, once the last has finished.
    bool going = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.transfer.outgoing && e.transfer.peer == peer &&
               (e.transfer.state == FileTransfer::State::Offered || e.transfer.state == FileTransfer::State::Sending);
    });
    if (going || inhibited_ || !offerNext) return out;

    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Waiting) continue;

        protocol::FileOffer offer;
        offer.size = (uint32_t)t.size;
        offer.digest = t.digest;
        offer.name = t.name;
        offer.kind = t.picture ? entry.kind : protocol::FILE_PLAIN;
        protocol::Record record = numbering.record(offer, time);
        if (!protocol::encodeRecord(record, out))
        {
            finish(entry, FileTransfer::State::Failed, "\"" + t.name + "\" could not be offered.");
            continue;
        }
        entry.offer = record.header.seq;
        t.state = FileTransfer::State::Offered;
        t.atMs = nowMs;
        t.resumedFrom = 0;
        t.done = 0;
        entry.sinceMs = nowMs;
        entry.written = 0;
        changed();
        break;
    }
    return out;
}

bool SessionFiles::takePiece(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset,
                             uint64_t nowMs, std::vector<uint8_t>& out, size_t maxBytes)
{
    if (inhibited_) return false;
    uint32_t time = unixSeconds(nowMs);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Sending) continue;
        if (entry.written >= t.size) return false;

        size_t most = std::max<size_t>(1, std::min(maxBytes, PIECE_BYTES));
        size_t length = (size_t)std::min<uint64_t>(most, t.size - entry.written);
        protocol::FileData data;
        data.offset = (uint32_t)entry.written;
        data.bytes.resize(length);
        entry.in.read((char*)data.bytes.data(), (std::streamsize)length);
        if (!entry.in || (size_t)entry.in.gcount() != length)
        {
            // Changed or gone since it was offered.
            protocol::FileCancel cancel;
            cancel.offer = entry.offer;
            cancel.reason = (uint8_t)CancelReason::SenderFailed;
            protocol::encodeRecord(numbering.record(cancel, time), out);
            finish(entry, FileTransfer::State::Failed, "\"" + t.path + "\" could not be read.");
            return true;
        }

        std::vector<uint8_t> record = protocol::encodeRecord(numbering.record(std::move(data), time, entry.offer));
        out.insert(out.end(), record.begin(), record.end());
        entry.pieces.push_back({streamOffset, streamOffset + record.size() - length, entry.written, length});
        entry.written += length;
        return true;
    }
    return false;
}

bool SessionFiles::advance(std::deque<Piece>& pieces, uint64_t streamAcked, uint64_t& done)
{
    uint64_t was = done;
    while (!pieces.empty() && pieces.front().streamStart + pieces.front().length <= streamAcked)
    {
        done = pieces.front().fileStart + pieces.front().length;
        pieces.pop_front();
    }
    // Into the piece now going: to the step, so a fast modem doesn't
    // redraw the screen for every frame.
    if (!pieces.empty() && streamAcked > pieces.front().streamStart)
    {
        const Piece& piece = pieces.front();
        uint64_t into = (streamAcked - piece.streamStart) / PROGRESS_STEP * PROGRESS_STEP;
        done = std::max(done, piece.fileStart + into);
    }
    return done != was;
}

void SessionFiles::acknowledged(const std::string& peer, uint64_t streamAcked)
{
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Sending) continue;
        if (advance(entry.pieces, streamAcked, t.done)) changed();
    }
    // Parts sent, still going out after a cancel.
    for (Kept& k : kept_)
    {
        if (!k.outgoing || k.peer != peer || k.pieces.empty()) continue;
        if (advance(k.pieces, streamAcked, k.bytes)) changed();
        k.handed = std::max(k.handed, k.bytes);
        if (k.pieces.empty())
        {
            k.handed = k.bytes;
            changed();
        }
    }
}

void SessionFiles::cut(const std::string& peer, uint64_t streamFrom)
{
    auto gone = [&](std::deque<Piece>& pieces) {
        auto first = std::find_if(pieces.begin(), pieces.end(),
                                  [&](const Piece& p) { return p.recordStart >= streamFrom; });
        if (first == pieces.end()) return false;
        pieces.erase(first, pieces.end());
        return true;
    };
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.peer != peer || t.state != FileTransfer::State::Sending) continue;
        uint64_t from = entry.written;
        for (const Piece& p : entry.pieces)
        {
            if (p.recordStart >= streamFrom)
            {
                from = p.fileStart;
                break;
            }
        }
        if (!gone(entry.pieces)) continue;
        // The file goes again from there.
        entry.written = from;
        entry.in.clear();
        entry.in.seekg((std::streamoff)from);
        changed();
    }
    for (Kept& k : kept_)
    {
        if (!k.outgoing || k.peer != peer || !gone(k.pieces)) continue;
        k.handed = k.pieces.empty() ? k.bytes : std::max(k.bytes, k.pieces.back().fileStart + k.pieces.back().length);
        changed();
    }
}

void SessionFiles::arriving(const std::string& peer, uint32_t offer, uint32_t offset, const uint8_t* data,
                            size_t bytes)
{
    Entry* entry = findIncoming(peer, offer);
    if (entry == nullptr || entry->transfer.state != FileTransfer::State::Receiving) return;
    entry->arrivingAt = offset;
    entry->arrivingData.assign(data, data + bytes);
    uint64_t stepped = bytes / PROGRESS_STEP * PROGRESS_STEP;
    if (stepped == entry->arriving) return;
    entry->arriving = stepped;
    changed();
}

void SessionFiles::autoAccept(Entry& entry)
{
    // A picture small enough shows as it comes, from anyone; other files
    // only from the stations listed.
    bool fromListed = !autoFolder_.empty() && autoCalls_.count(entry.transfer.peer) != 0;
    bool picture = entry.transfer.picture && !pictureFolder_.empty() && entry.transfer.size <= pictureMax_;
    if (inhibited_ || (!fromListed && !picture)) return;
    const std::string& into = picture ? pictureFolder_ : autoFolder_;
    if (!entry.transfer.keptPath.empty() && resumePart(entry, entry.transfer.keptPath))
    {
        entry.transfer.autoAccepted = true;
        return;
    }

    std::error_code ec;
    std::filesystem::path folder = pathFromUtf8(into);
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path chosen = freeSavePath(folder, entry.transfer.name);
    std::string path = utf8FromPath(chosen);
    std::string error = "No free name for \"" + entry.transfer.name + "\" in \"" + into + "\".";
    if (!chosen.empty() && openPart(entry, path, error))
    {
        entry.transfer.autoAccepted = true;
    }
    else
    {
        // Not written: the operator is asked instead.
        entry.transfer.error = error;
        entry.transfer.state = FileTransfer::State::Asking;
        if (entry.out.is_open()) entry.out.close();
    }
}

void SessionFiles::onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs)
{
    using State = FileTransfer::State;
    nowMs_ = nowMs;

    if (const auto* offer = record.as<protocol::FileOffer>())
    {
        uint32_t number = record.header.seq;
        if (findIncoming(peer, number) != nullptr) return;
        Entry& entry = entries_.emplace_back();
        FileTransfer& t = entry.transfer;
        t.id = nextId_++;
        t.outgoing = false;
        t.peer = peer;
        t.size = offer->size;
        t.digest = offer->digest;
        t.name = safeFileName(offer->name);
        t.picture = protocol::isPictureKind(offer->kind);
        t.state = State::Asking;
        t.atMs = nowMs;
        entry.offer = number;
        entry.sinceMs = nowMs;
        auto kept = findKept(entry);
        if (kept != kept_.end())
        {
            t.kept = kept->bytes;
            t.keptPath = kept->path;
        }
        changed();
        if (inhibited_)
        {
            // It could not be answered: the far end hears so at once.
            queueCancel(entry, CancelReason::ReceiverFailed);
            finish(entry, State::Failed, NOT_ALLOWED);
        }
        else if (t.size > MAX_FILE_BYTES)
        {
            queueCancel(entry, CancelReason::ReceiverFailed);
            finish(entry, State::Failed, peer + " offered more than the 300 KB Ragtime takes over HF.");
        }
        else
        {
            autoAccept(entry);
        }
        prune();
    }
    else if (const auto* answer = record.as<protocol::FileAnswer>())
    {
        Entry* entry = findOutgoing(peer, answer->offer);
        if (entry == nullptr || entry->transfer.state != State::Offered) return;
        FileTransfer& t = entry->transfer;
        if (!answer->accept)
        {
            finish(*entry, State::Declined);
            return;
        }
        uint64_t from = answer->from;
        if (from > t.size)
        {
            queueCancel(*entry, CancelReason::SenderFailed);
            finish(*entry, State::Failed, t.peer + " asked to carry on past the end of the file.");
            return;
        }
        entry->in.open(pathFromUtf8(t.path), std::ios::binary);
        if (entry->in && from > 0) entry->in.seekg((std::streamoff)from);
        if (!entry->in)
        {
            queueCancel(*entry, CancelReason::SenderFailed);
            finish(*entry, State::Failed, "\"" + t.path + "\" could not be opened.");
            return;
        }
        entry->written = from;
        t.done = from;
        t.resumedFrom = from;
        t.state = State::Sending;
        changed();
    }
    else if (const auto* data = record.as<protocol::FileData>())
    {
        // Anything for a transfer not under way, such as pieces still
        // coming after it was cancelled, is dropped.
        Entry* entry = findIncoming(peer, record.header.seq);
        if (entry == nullptr || entry->transfer.state != State::Receiving) return;
        receive(*entry, *data);
    }
    else if (const auto* saved = record.as<protocol::FileSaved>())
    {
        Entry* entry = findOutgoing(peer, saved->offer);
        if (entry == nullptr)
        {
            // Saved before our cancel of it arrived: it was delivered.
            for (Entry& e : entries_)
            {
                const FileTransfer& t = e.transfer;
                if (e.awaitingSaved && t.outgoing && t.peer == peer && e.offer == saved->offer) entry = &e;
            }
            if (entry == nullptr) return;
            entry->awaitingSaved = false;
            for (auto it = kept_.begin(); it != kept_.end(); ++it)
            {
                if (it->outgoing && it->transferId == entry->transfer.id)
                {
                    endSent(it, true);
                    break;
                }
            }
        }
        else if (entry->transfer.state != State::Sending)
        {
            return;
        }
        entry->transfer.done = entry->transfer.size;
        finish(*entry, State::Delivered);
    }
    else if (const auto* cancel = record.as<protocol::FileCancel>())
    {
        // The reason's top bit says whose transfer it ends: the receiver's
        // reasons end one of ours.
        bool fromReceiver = (cancel->reason & protocol::CANCEL_FROM_RECEIVER) != 0;
        Entry* entry = fromReceiver ? findOutgoing(peer, cancel->offer) : findIncoming(peer, cancel->offer);
        if (entry == nullptr) return;
        if (cancel->reason == (uint8_t)CancelReason::ReceiverHasEnough && entry->transfer.state == State::Sending)
        {
            // The far end kept what came of it: delivered, as far as it
            // went. What the host still holds needn't go.
            if (entry->written > entry->transfer.done) cutsWanted_++;
            entry->transfer.enough = true;
            finish(*entry, State::Delivered);
            return;
        }

        State state = State::CancelledThere;
        if (cancel->reason == (uint8_t)CancelReason::SenderExpired ||
            cancel->reason == (uint8_t)CancelReason::ReceiverExpired)
        {
            state = State::Expired;
        }
        else if (cancel->reason == (uint8_t)CancelReason::SenderFailed ||
                 cancel->reason == (uint8_t)CancelReason::ReceiverFailed)
        {
            state = State::FailedThere;
        }
        finish(*entry, state);
    }
}

void SessionFiles::receive(Entry& entry, const protocol::FileData& data)
{
    FileTransfer& t = entry.transfer;
    size_t length = data.bytes.size();
    if (data.offset != t.done || t.done + length > t.size)
    {
        entry.spoiled = true;
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed,
               data.offset != t.done ? t.peer + " sent the file out of order." : t.peer + " sent more than it offered.");
        return;
    }

    entry.out.write((const char*)data.bytes.data(), (std::streamsize)length);
    // A picture is read back as it comes, to show it.
    if (entry.transfer.picture) entry.out.flush();
    if (!entry.out)
    {
        entry.spoiled = true;
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + utf8FromPath(entry.part) + "\" could not be written.");
        return;
    }
    t.done += length;
    entry.arriving = 0;
    entry.arrivingData.clear();
    changed();
    if (t.done == t.size) completeReceive(entry);
}

// All of it has come: the part file becomes the file, once its digest
// checks, and the sender is told it is saved.
void SessionFiles::completeReceive(Entry& entry)
{
    FileTransfer& t = entry.transfer;
    entry.out.close();
    bool written = !entry.out.fail();
    protocol::Digest digest{};
    if (written && (!fileDigest(entry.part, t.size, digest) || digest != t.digest))
    {
        // Not the file offered: pieces from a changed file, say, after
        // carrying on from a kept part.
        entry.spoiled = true;
        std::error_code ec;
        std::filesystem::remove(entry.part, ec);
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + t.name + "\" did not match what " + t.peer + " sent.");
        return;
    }

    if (!written || !placePart(entry))
    {
        std::error_code ec;
        std::filesystem::remove(entry.part, ec);
        queueCancel(entry, CancelReason::ReceiverFailed);
        finish(entry, FileTransfer::State::Failed, "\"" + t.path + "\" could not be saved.");
        return;
    }

    protocol::FileSaved saved;
    saved.offer = entry.offer;
    queue(t.peer, saved);
    finish(entry, FileTransfer::State::Saved);
}

bool SessionFiles::placePart(Entry& entry)
{
    FileTransfer& t = entry.transfer;
    std::filesystem::path final = pathFromUtf8(t.path);
    std::filesystem::path part = entry.part;
    std::error_code ec;
    // Another transfer's part file is never replaced.
    auto clash = [&](const std::filesystem::path& path) {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const Entry& e) { return &e != &entry && !e.part.empty() && e.part == path; });
    };
    if (t.autoAccepted && (std::filesystem::exists(final, ec) || clash(final)))
    {
        // Taken while it came: auto-accept never replaces a file, so it
        // gets the next free name.
        std::filesystem::path other = freeSavePath(final.parent_path(), t.name);
        if (other.empty() || clash(other)) return false;
        final = other;
        t.path = utf8FromPath(final);
    }
    else if (clash(final))
    {
        return false;
    }
    else if (std::filesystem::exists(final, ec))
    {
        // The operator agreed, in the save dialog, to replace it.
        std::filesystem::remove(final, ec);
    }
    std::filesystem::rename(part, final, ec);
    return !ec;
}

void SessionFiles::notTaken(const std::string& peer)
{
    for (Entry& entry : entries_)
    {
        if (entry.transfer.outgoing && entry.transfer.peer == peer && entry.transfer.state == FileTransfer::State::Waiting)
        {
            finish(entry, FileTransfer::State::NotTaken, peer + "'s client takes no files.");
        }
    }
}

void SessionFiles::sessionEnded(const std::string& peer, uint64_t nowMs)
{
    nowMs_ = nowMs;
    pending_.erase(peer);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (t.peer == peer) entry.awaitingSaved = false;
        if (t.peer != peer || !t.live()) continue;
        if (t.state == FileTransfer::State::Waiting) entry.sinceMs = nowMs; // for the next session
        else finish(entry, FileTransfer::State::Failed, "The session with " + peer + " ended.");
    }
    stopSent(peer);
}

void SessionFiles::noSession(const std::string& peer, const std::string& why)
{
    for (Entry& entry : entries_)
    {
        if (entry.transfer.peer == peer && entry.transfer.state == FileTransfer::State::Waiting)
        {
            finish(entry, FileTransfer::State::Failed, why);
        }
    }
}

void SessionFiles::release(const std::string& peer, bool inSession)
{
    using State = FileTransfer::State;
    if (!inSession) pending_.erase(peer);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (t.peer == peer) entry.awaitingSaved = false;
        if (t.peer != peer || !t.live()) continue;

        // One offered in the session is cancelled there too, so the far
        // end hears what became of it.
        if (inSession && t.state != State::Waiting)
        {
            queueCancel(entry, t.outgoing ? CancelReason::SenderStopped : CancelReason::ReceiverStopped);
        }
        finish(entry, State::Cancelled);
    }
}

void SessionFiles::stopAll(const std::string& why)
{
    pending_.clear();
    for (Entry& entry : entries_)
    {
        entry.awaitingSaved = false;
        if (entry.transfer.live()) finish(entry, FileTransfer::State::Failed, why);
    }
    stopSent(std::string());
}

void SessionFiles::tick(uint64_t nowMs)
{
    using State = FileTransfer::State;
    nowMs_ = nowMs;
    limitKept(nowMs);
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if ((t.state != State::Offered && t.state != State::Asking) || nowMs - entry.sinceMs < OFFER_EXPIRY_MS)
        {
            continue;
        }
        queueCancel(entry, t.outgoing ? CancelReason::SenderExpired : CancelReason::ReceiverExpired);
        finish(entry, State::Expired);
    }
}

void SessionFiles::setInhibited(bool inhibited)
{
    if (inhibited == inhibited_) return;
    inhibited_ = inhibited;
    if (!inhibited) return;

    using State = FileTransfer::State;
    for (Entry& entry : entries_)
    {
        FileTransfer& t = entry.transfer;
        if (!t.live()) continue;
        if (t.state != State::Waiting)
        {
            // As for a cancel: one written in full may be saved there already.
            entry.awaitingSaved = t.state == State::Sending && entry.written >= t.size;
            queueCancel(entry, t.outgoing ? CancelReason::SenderFailed : CancelReason::ReceiverFailed);
        }
        finish(entry, State::Failed, NOT_ALLOWED);
    }
}

const FileTransfer* SessionFiles::transfer(uint64_t id) const
{
    for (const Entry& entry : entries_)
    {
        if (entry.transfer.id == id) return &entry.transfer;
    }
    return nullptr;
}

bool SessionFiles::reorder(uint64_t id, int delta)
{
    auto isQueued = [](const Entry& e, const std::string& peer) {
        return e.transfer.outgoing && e.transfer.peer == peer && e.transfer.state == FileTransfer::State::Waiting;
    };
    auto it = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.transfer.id == id; });
    if (it == entries_.end() || delta == 0) return false;
    const std::string peer = it->transfer.peer;
    if (!isQueued(*it, peer)) return false;
    if (delta < 0)
    {
        // Before the queued one ahead of it.
        auto before = entries_.end();
        for (auto j = entries_.begin(); j != it; ++j)
        {
            if (isQueued(*j, peer)) before = j;
        }
        if (before == entries_.end()) return false;
        entries_.splice(before, entries_, it);
    }
    else
    {
        // After the queued one behind it.
        auto after = std::next(it);
        while (after != entries_.end() && !isQueued(*after, peer)) ++after;
        if (after == entries_.end()) return false;
        entries_.splice(std::next(after), entries_, it);
    }
    changed();
    return true;
}

std::vector<KeptPart> SessionFiles::keptParts() const
{
    std::vector<KeptPart> out;
    for (const Kept& k : kept_)
    {
        KeptPart p;
        p.id = k.id;
        p.outgoing = k.outgoing;
        p.peer = k.peer;
        p.name = !k.name.empty() ? k.name : utf8FromPath(pathFromUtf8(k.path).filename());
        p.size = k.size;
        std::copy(k.digest.begin(), k.digest.end(), p.digest);
        p.path = k.path;
        p.part = utf8FromPath(k.part);
        p.bytes = k.bytes;
        p.handed = std::max(k.handed, k.bytes);
        p.keptAtMs = k.keptAtMs;
        p.keepUntilMs = k.keepUntilMs != 0 ? k.keepUntilMs : k.keptAtMs + KEPT_MS;
        p.hasEnding = k.hasEnding;
        p.ending = k.ending;
        out.push_back(std::move(p));
    }
    return out;
}

void SessionFiles::restoreKept(const std::vector<KeptPart>& parts, uint64_t nowMs)
{
    for (const KeptPart& p : parts)
    {
        uint64_t until = p.keepUntilMs != 0 ? p.keepUntilMs : p.keptAtMs + KEPT_MS;
        std::filesystem::path part;
        if (p.outgoing)
        {
            // A part sent: the far end's is its own to keep.
            if (p.peer.empty() || p.bytes == 0 || p.bytes > p.size || nowMs >= until)
            {
                if (p.hasEnding) ended_.push_back(p.ending);
                continue;
            }
            bool known = std::any_of(kept_.begin(), kept_.end(), [&](const Kept& k) {
                return k.outgoing && k.peer == protocol::normalCallsign(p.peer) && k.size == p.size
                       && std::equal(k.digest.begin(), k.digest.end(), p.digest);
            });
            if (known) continue;
        }
        else
        {
            part = pathFromUtf8(p.part);
            std::error_code ec;
            uint64_t bytes = std::filesystem::file_size(part, ec);
            if (ec || bytes != p.bytes || p.bytes == 0 || p.bytes >= p.size || nowMs >= until)
            {
                // Gone, changed or too old: whatever is left of it goes, and
                // the file with it.
                if (!ec && nowMs >= until) std::filesystem::remove(part, ec);
                if (p.hasEnding) ended_.push_back(p.ending);
                continue;
            }
            bool known = std::any_of(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.part == part; });
            if (known) continue;
        }
        Kept& k = kept_.emplace_back();
        k.id = nextId_++;
        k.outgoing = p.outgoing;
        k.peer = protocol::normalCallsign(p.peer);
        k.name = p.name;
        k.size = p.size;
        std::copy(p.digest, p.digest + protocol::DIGEST_BYTES, k.digest.begin());
        k.path = p.path;
        k.part = part;
        k.bytes = p.bytes;
        k.handed = p.bytes;
        k.keptAtMs = p.keptAtMs;
        k.keepUntilMs = until;
        k.hasEnding = p.hasEnding;
        k.ending = p.ending;
    }
    limitKept(nowMs);
    changed();
}

bool SessionFiles::keepLonger(uint64_t keptId, uint64_t ms, uint64_t nowMs)
{
    for (Kept& k : kept_)
    {
        if (k.id != keptId) continue;
        uint64_t until = k.keepUntilMs != 0 ? k.keepUntilMs : k.keptAtMs + KEPT_MS;
        k.keepUntilMs = std::max(until, nowMs) + ms;
        changed();
        return true;
    }
    return false;
}

bool SessionFiles::deleteKept(uint64_t keptId)
{
    auto it = std::find_if(kept_.begin(), kept_.end(), [keptId](const Kept& k) { return k.id == keptId; });
    if (it == kept_.end()) return false;
    dropKept(it, true);
    return true;
}

void SessionFiles::setKeptEnding(uint64_t transferId, const FinishedFile& ending)
{
    for (Kept& k : kept_)
    {
        if (k.transferId != transferId) continue;
        k.hasEnding = true;
        k.ending = ending;
        changed();
        return;
    }
}

std::vector<FinishedFile> SessionFiles::takeEnded()
{
    std::vector<FinishedFile> out;
    out.swap(ended_);
    return out;
}

bool SessionFiles::hasRecords(const std::string& peer) const
{
    auto pending = pending_.find(peer);
    return pending != pending_.end() && !pending->second.empty();
}

bool SessionFiles::wantsSession(std::string& peer, uint64_t& sinceMs) const
{
    if (inhibited_) return false;
    bool found = false;
    for (const Entry& entry : entries_)
    {
        const FileTransfer& t = entry.transfer;
        if (!t.outgoing || t.state != FileTransfer::State::Waiting) continue;
        if (!found || entry.sinceMs < sinceMs)
        {
            peer = t.peer;
            sinceMs = entry.sinceMs;
            found = true;
        }
    }
    return found;
}

bool SessionFiles::busyWith(const std::string& peer) const
{
    if (hasRecords(peer)) return true;
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const Entry& e) { return e.transfer.peer == peer && e.transfer.live(); });
}

} // namespace files
} // namespace ragtime
