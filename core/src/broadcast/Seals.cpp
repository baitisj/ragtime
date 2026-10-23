#include "ragtime/broadcast/Seals.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "ragtime/broadcast/Airtime.h"

namespace ragtime
{
namespace broadcast
{

namespace
{

using Event = link::HostLink::Event;
using BurstState = link::HostLink::BurstState;

constexpr uint16_t MAX_SEAL_NUMBER = 0xFFFF;
// Seals kept for showing and answering, at most.
constexpr size_t MAX_SEALS_KEPT = 200;
// How often the listener's memory is tidied.
constexpr uint64_t FORGET_EVERY_MS = 10 * 1000;

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

bool zero(const protocol::Digest& digest)
{
    return std::all_of(digest.begin(), digest.end(), [](uint8_t b) { return b == 0; });
}

uint16_t before(uint16_t number)
{
    return number <= 1 ? MAX_SEAL_NUMBER : (uint16_t)(number - 1);
}

bool covers(const protocol::Seal& seal, uint32_t seq)
{
    return seq >= seal.firstItem && seq < seal.firstItem + seal.items.size();
}

} // namespace

bool Seals::Lack::any() const
{
    return wantSeal || std::find(items.begin(), items.end(), true) != items.end() || !pieces.empty();
}

size_t Seals::Lack::size() const
{
    size_t n = wantSeal ? 1 : 0;
    n += (size_t)std::count(items.begin(), items.end(), true);
    for (const auto& file : pieces) n += file.second.size();
    return n;
}

Seals::Seals(link::HostLink& link, BroadcastChat& chat)
    : link_(link)
    , chat_(chat)
    , random_(std::random_device{}())
{
}

void Seals::setChain(const std::string& callsign, const Chain& chain)
{
    std::string call = protocol::normalCallsign(callsign);
    if (call != callsign_)
    {
        // Another station's numbering: nothing of ours carries over.
        unsealed_.clear();
        sealedItems_.clear();
        seals_.clear();
        windowEndsMs_ = 0;
        asked_.clear();
        resendBursts_.clear();
        resendLast_ = 0;
        sealSoon_ = false;
        callsign_ = call;
    }
    if (seals_.empty())
    {
        lastNumber_ = chain.number;
        lastDigest_ = chain.digest;
    }
    version_++;
}

link::ModeInfo Seals::modeInfo(const std::string& mode) const
{
    return findMode(link_.modes(), mode, link_.groupMode());
}

//-------------------------------------------------------------------------
// Ours
//-------------------------------------------------------------------------

void Seals::going(const std::vector<protocol::Record>& records, uint64_t burst, const std::string& mode,
                  uint64_t nowMs)
{
    bool tracked = false;
    for (const protocol::Record& r : records)
    {
        auto type = (protocol::RecordType)r.type();
        if (!protocol::isItem(type)) continue;
        uint32_t seq = r.header.seq;

        bool known = false;
        for (OurItem& item : unsealed_)
        {
            if (item.seq != seq || item.record.header.time != r.header.time) continue;
            // Sent again: it waits for this burst now.
            item.burst = burst;
            item.fate = Fate::Going;
            item.mode = mode;
            known = true;
        }
        auto sealed = sealedItems_.find(seq);
        if (sealed != sealedItems_.end() && sealed->second.record.header.time == r.header.time) known = true;
        if (known)
        {
            tracked = true;
            continue;
        }
        // Chat goes unsealed unless the operator asked for sealed chat.
        if (!sealText_ && type == protocol::RecordType::Text) continue;
        tracked = true;

        OurItem item;
        item.seq = seq;
        item.record = r;
        item.digest = protocol::recordDigest(r);
        item.burst = burst;
        item.mode = mode;
        item.file = type == protocol::RecordType::FileAnnounce;
        link::ModeInfo info = modeInfo(mode);
        std::vector<uint8_t> frame = protocol::encodeFrame({r});
        item.air = info.valid() ? info.burstSeconds({(int)frame.size()}) : UNKNOWN_BURST_SECONDS;
        unsealed_.push_back(std::move(item));
    }
    // Unsealed chat neither delays a seal nor changes what the screen shows.
    if (!tracked) return;
    lastItemMs_ = nowMs;
    version_++;
}

void Seals::addAirtime(uint32_t seq, double seconds)
{
    for (OurItem& item : unsealed_)
    {
        if (item.seq == seq) item.air += seconds;
    }
    auto sealed = sealedItems_.find(seq);
    if (sealed != sealedItems_.end()) sealed->second.air += seconds;
}

const Seals::OurItem* Seals::findItem(uint32_t seq) const
{
    auto it = sealedItems_.find(seq);
    if (it != sealedItems_.end()) return &it->second;
    for (const OurItem& item : unsealed_)
    {
        if (item.seq == seq) return &item;
    }
    return nullptr;
}

Seals::OurSeal* Seals::findSeal(uint16_t number)
{
    for (OurSeal& s : seals_)
    {
        if (s.number == number) return &s;
    }
    return nullptr;
}

const Seals::OurSeal* Seals::sealFor(uint32_t seq) const
{
    for (auto it = seals_.rbegin(); it != seals_.rend(); ++it)
    {
        if (seq >= it->first && seq < it->first + it->count) return &*it;
    }
    return nullptr;
}

bool Seals::newest(const OurSeal& seal) const
{
    size_t seen = 0;
    for (auto it = seals_.rbegin(); it != seals_.rend() && seen < NEWEST_OPEN; ++it, ++seen)
    {
        if (&*it == &seal) return true;
    }
    return false;
}

bool Seals::open(const OurSeal& seal, uint64_t nowMs) const
{
    if (seal.closed || !seal.sent) return false;
    if (seal.keepUntilMs != 0) return nowMs < seal.keepUntilMs;
    uint64_t age = nowMs >= seal.sentMs ? nowMs - seal.sentMs : 0;
    if (age >= MAX_OPEN_MS) return false;
    if (newest(seal)) return true;
    return (double)age < OPEN_AIRTIMES * seal.air * 1000.0;
}

uint64_t Seals::openUntil(const OurSeal& seal, uint64_t nowMs) const
{
    if (!open(seal, nowMs)) return 0;
    if (seal.keepUntilMs != 0) return seal.keepUntilMs;
    if (newest(seal)) return seal.sentMs + MAX_OPEN_MS;
    uint64_t byAir = (uint64_t)(OPEN_AIRTIMES * seal.air * 1000.0);
    return seal.sentMs + std::min(MAX_OPEN_MS, byAir);
}

Seals::SealView Seals::sealOf(uint32_t seq, uint64_t nowMs) const
{
    SealView view;
    const OurSeal* s = sealFor(seq);
    if (s == nullptr) return view;
    view.sealed = s->sent;
    view.number = s->number;
    view.open = open(*s, nowMs);
    view.openUntilMs = openUntil(*s, nowMs);
    view.keptOpen = s->keepUntilMs != 0;
    view.askers = s->askers.size();
    view.asks = s->asks;
    bool asked = asked_.count(s->number) != 0;
    view.resending = resendLast_ != 0 || (asked && windowEndsMs_ != 0);
    return view;
}

bool Seals::keepOpen(uint32_t seq, uint64_t untilMs)
{
    const OurSeal* found = sealFor(seq);
    if (found == nullptr || !found->sent) return false;
    OurSeal* s = findSeal(found->number);
    s->keepUntilMs = untilMs;
    s->closed = false;
    version_++;
    return true;
}

bool Seals::closeNow(uint32_t seq)
{
    const OurSeal* found = sealFor(seq);
    if (found == nullptr) return false;
    OurSeal* s = findSeal(found->number);
    s->closed = true;
    s->keepUntilMs = 0;
    asked_.erase(s->number);
    version_++;
    return true;
}

void Seals::sealIfDue(uint64_t nowMs)
{
    if (link_.paused() || resendLast_ != 0 || callsign_.empty()) return;
    // One seal burst at a time, so the chain goes out in order.
    for (const OurSeal& s : seals_)
    {
        if (!s.sent && s.tries < SEAL_TRIES) return;
    }
    while (!unsealed_.empty() && unsealed_.front().fate == Fate::Unsent) unsealed_.pop_front();
    if (unsealed_.empty())
    {
        sealSoon_ = false;
        return;
    }

    // The settled items at the front. A seal lists consecutive numbers, so
    // a gap (unsealed chat, a number that never reached us, or the numbers
    // wrapping) starts another seal, made now and sent in the same bursts.
    size_t n = 0;
    bool anySent = false;
    uint64_t oldestSent = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < unsealed_.size(); i++)
    {
        const OurItem& item = unsealed_[i];
        if (item.fate == Fate::Going) break;
        if (item.file && files_ != nullptr && files_->streaming(item.seq)) break;
        if (item.fate == Fate::Sent)
        {
            anySent = true;
            oldestSent = std::min(oldestSent, item.sentMs);
        }
        n++;
    }
    if (n == 0 || !anySent) return;
    bool going = std::any_of(unsealed_.begin(), unsealed_.end(), [](const OurItem& i) { return i.fate == Fate::Going; });
    bool due = (!going && (sealSoon_ || nowMs >= lastItemMs_ + QUIET_MS)) || nowMs >= oldestSent + SEAL_EVERY_MS;
    if (!due) return;
    sealSoon_ = false;

    uint32_t time = seconds(nowMs);
    std::vector<OurSeal> made;
    size_t at = 0;
    while (at < n)
    {
        // A run after a gap starts after its items that never went, as the
        // first run does.
        bool gap = at > 0 && unsealed_[at].seq != unsealed_[at - 1].seq + 1;
        if (gap && unsealed_[at].fate == Fate::Unsent)
        {
            size_t skip = at;
            while (skip < n && unsealed_[skip].fate == Fate::Unsent &&
                   (skip == at || unsealed_[skip].seq == unsealed_[skip - 1].seq + 1))
                skip++;
            at = skip;
            continue;
        }
        size_t end = at + 1;
        while (end < n && unsealed_[end].seq == unsealed_[end - 1].seq + 1) end++;
        size_t k = std::min<size_t>(end - at, 255);
        protocol::Seal seal;
        protocol::Record record;
        uint16_t number = lastNumber_ >= MAX_SEAL_NUMBER ? 1 : (uint16_t)(lastNumber_ + 1);
        for (;;)
        {
            seal = protocol::Seal();
            seal.number = number;
            seal.previous = lastDigest_;
            seal.firstItem = unsealed_[at].seq;
            for (size_t i = at; i < at + k; i++)
                seal.items.push_back(unsealed_[i].fate == Fate::Sent ? unsealed_[i].digest : protocol::Digest{});
            record = chat_.record(seal, time);
            std::vector<uint8_t> frame = protocol::encodeFrame({record});
            if ((!frame.empty() && frame.size() <= protocol::MAX_BROADCAST_FRAME) || k == 1) break;
            k--;
        }
        OurSeal s;
        s.number = number;
        s.record = record;
        s.digest = protocol::recordDigest(record);
        s.first = seal.firstItem;
        s.count = k;
        for (size_t i = at; i < at + k; i++)
        {
            s.air += unsealed_[i].air;
            s.mode = unsealed_[i].mode;
            sealedItems_[unsealed_[i].seq] = unsealed_[i];
        }
        lastNumber_ = number;
        lastDigest_ = s.digest;
        made.push_back(std::move(s));
        at += k;
    }
    unsealed_.erase(unsealed_.begin(), unsealed_.begin() + (std::ptrdiff_t)n);

    // A seal a frame, the frames as bursts.
    std::string mode = made.back().mode;
    std::vector<std::vector<uint8_t>> frames;
    for (const OurSeal& s : made) frames.push_back(protocol::encodeFrame({s.record}));
    size_t index = 0;
    for (const auto& burst : packBursts(frames, modeInfo(mode)))
    {
        uint64_t id = link_.broadcast(burst, mode, nowMs, burst.size() == 1 ? std::string("seal")
                                                                            : std::to_string(burst.size()) + " seals");
        for (size_t i = 0; i < burst.size(); i++, index++)
        {
            made[index].burst = id;
            made[index].tries = 1;
        }
    }
    for (OurSeal& s : made) seals_.push_back(std::move(s));
    version_++;
}

void Seals::onRequest(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs)
{
    OurSeal* s = findSeal(request.seal);
    if (s == nullptr) return;
    if (!zero(request.sealDigest) && request.sealDigest != s->digest) return;
    if (!open(*s, nowMs)) return;

    Asked& a = asked_[request.seal];
    if (request.wantSeal || zero(request.sealDigest)) a.seal = true;
    for (size_t i = 0; i < request.missingItems.size() && i < s->count; i++)
    {
        if (!request.missingItems[i]) continue;
        const OurItem* item = findItem(s->first + (uint32_t)i);
        if (item != nullptr && item->fate == Fate::Sent) a.items.insert(item->seq);
    }
    for (const auto& file : request.files)
    {
        uint32_t seq = file.first;
        if (seq < s->first || seq >= s->first + s->count || files_ == nullptr) continue;
        uint16_t pieces = files_->pieceCount(seq);
        for (uint16_t p : file.second)
        {
            if (p < pieces) a.pieces[seq].insert(p);
        }
    }
    if (!a.seal && a.items.empty() && a.pieces.empty()) asked_.erase(request.seal);
    s->askers[from] = nowMs;
    s->asks++;
    // A request with no window open, from a late joiner: a window now, so
    // other requests can join it.
    if (windowEndsMs_ == 0 && resendLast_ == 0) windowEndsMs_ = nowMs + listeningWindowMs(modeInfo(s->mode));
    version_++;
}

void Seals::resend(uint64_t nowMs)
{
    rounds_++;
    // The union of what was asked, in sequence order: items, and each
    // file's pieces after its announce.
    std::set<uint32_t> items;
    std::map<uint32_t, std::set<uint16_t>> pieces;
    std::vector<const OurSeal*> seals;
    for (const auto& entry : asked_)
    {
        const OurSeal* s = findSeal(entry.first);
        if (s == nullptr) continue;
        seals.push_back(s);
        items.insert(entry.second.items.begin(), entry.second.items.end());
        for (const auto& file : entry.second.pieces)
            pieces[file.first].insert(file.second.begin(), file.second.end());
    }
    asked_.clear();
    if (seals.empty()) return;
    std::sort(seals.begin(), seals.end(), [](const OurSeal* a, const OurSeal* b) { return a->number < b->number; });

    std::set<uint32_t> order = items;
    for (const auto& file : pieces) order.insert(file.first);
    uint32_t time = seconds(nowMs);
    std::vector<protocol::Record> records;
    for (uint32_t seq : order)
    {
        if (items.count(seq) != 0)
        {
            if (const OurItem* item = findItem(seq)) records.push_back(item->record);
        }
        auto file = pieces.find(seq);
        if (file == pieces.end() || files_ == nullptr) continue;
        for (uint16_t index : file->second)
        {
            protocol::Record piece;
            if (files_->piece(seq, index, time, piece)) records.push_back(std::move(piece));
        }
    }

    std::string mode = seals.back()->mode;
    link::ModeInfo info = modeInfo(mode);
    std::vector<std::vector<std::vector<uint8_t>>> bursts = packBursts(packFrames(records), info);
    // Within this round's airtime; the rest is asked for again.
    double air = 0.0;
    size_t keep = 0;
    while (keep < bursts.size())
    {
        air += burstSeconds(bursts[keep], info);
        if (keep > 0 && air > RESEND_MAX_SECONDS) break;
        keep++;
    }
    bursts.resize(keep);
    // The seals last: hearing one again opens the next window.
    std::vector<std::vector<uint8_t>> sealFrames;
    for (const OurSeal* s : seals) sealFrames.push_back(protocol::encodeFrame({s->record}));
    for (auto& burst : packBursts(sealFrames, info)) bursts.push_back(std::move(burst));

    for (const auto& burst : bursts)
    {
        uint64_t id = link_.broadcast(burst, mode, nowMs, "repairs");
        if (id == 0) continue;
        resendBursts_.insert(id);
        resendLast_ = id;
    }
    version_++;
}

void Seals::flush(uint64_t nowMs)
{
    std::vector<OurSeal*> again;
    for (OurSeal& s : seals_)
    {
        if (!s.sent || s.flushes >= FLUSH_REPEATS || !open(s, nowMs)) continue;
        bool file = false;
        for (uint32_t seq = s.first; seq < s.first + s.count && !file; seq++)
        {
            const OurItem* item = findItem(seq);
            file = item != nullptr && item->file;
        }
        if (file) again.push_back(&s);
    }
    if (again.empty()) return;
    std::vector<std::vector<uint8_t>> frames;
    for (OurSeal* s : again)
    {
        frames.push_back(protocol::encodeFrame({s->record}));
        s->flushes++;
    }
    std::string mode = again.back()->mode;
    for (const auto& burst : packBursts(frames, modeInfo(mode)))
    {
        uint64_t id = link_.broadcast(burst, mode, nowMs, "seal again");
        if (id == 0) continue;
        resendBursts_.insert(id);
        resendLast_ = id;
    }
}

//-------------------------------------------------------------------------
// Events and records
//-------------------------------------------------------------------------

void Seals::onEvent(const Event& event, uint64_t nowMs)
{
    if (event.type != Event::Type::Burst) return;
    bool settled = event.burstState == BurstState::Sent || event.burstState == BurstState::NotSent;
    if (!settled) return;
    bool sent = event.burstState == BurstState::Sent;

    for (OurItem& item : unsealed_)
    {
        if (item.burst != event.id || item.fate != Fate::Going) continue;
        item.fate = sent ? Fate::Sent : Fate::Unsent;
        if (sent)
        {
            item.sentMs = nowMs;
            lastItemMs_ = nowMs;
        }
        version_++;
    }

    std::vector<OurSeal*> again;
    for (OurSeal& s : seals_)
    {
        if (s.burst != event.id || s.sent) continue;
        if (sent)
        {
            s.sent = true;
            s.sentMs = nowMs;
            sealsSent_++;
            windowEndsMs_ = std::max(windowEndsMs_, nowMs + listeningWindowMs(modeInfo(s.mode)));
        }
        else if (s.tries < SEAL_TRIES)
        {
            again.push_back(&s);
        }
        else
        {
            // Given up: answerable from now, but with no window of its own.
            s.sent = true;
            s.sentMs = nowMs;
        }
        version_++;
    }
    if (!again.empty())
    {
        std::vector<std::vector<uint8_t>> frames;
        for (OurSeal* s : again) frames.push_back(protocol::encodeFrame({s->record}));
        uint64_t id = link_.broadcast(frames, again.back()->mode, nowMs, "seal again");
        for (OurSeal* s : again)
        {
            s->burst = id;
            s->tries++;
        }
    }

    if (resendBursts_.erase(event.id) != 0 && event.id == resendLast_)
    {
        resendLast_ = 0;
        // The round's seals went again: listeners ask once more if they
        // still lack anything.
        if (sent)
        {
            const OurSeal* last = seals_.empty() ? nullptr : &seals_.back();
            windowEndsMs_ = nowMs + listeningWindowMs(modeInfo(last ? last->mode : mode_));
        }
        version_++;
    }
}

void Seals::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (from.empty() || from == callsign_) return;

    if (record.as<protocol::Seal>() != nullptr)
    {
        heardSeal(from, record, nowMs);
    }
    else if (const protocol::RepairRequest* request = record.as<protocol::RepairRequest>())
    {
        if (request->to == callsign_)
        {
            requestsHeard_++;
            onRequest(from, *request, nowMs);
        }
        else
        {
            heardRequest(from, *request, nowMs);
        }
    }
    else if (protocol::isItem((protocol::RecordType)record.type()))
    {
        HeardItem& item = stations_[from].items[record.header.seq];
        item.digest = protocol::recordDigest(record);
        item.type = record.type();
        item.heardMs = nowMs;
    }
    else if (record.as<protocol::FilePiece>() != nullptr)
    {
        stations_[from].piecesOf[record.header.seq] = nowMs;
    }
}

void Seals::tick(uint64_t nowMs)
{
    if (windowEndsMs_ != 0 && nowMs >= windowEndsMs_ && resendLast_ == 0)
    {
        windowEndsMs_ = 0;
        // Requests start another round; a window with none ends the
        // repairs, but for a file's seal, which goes again a time or two.
        if (!asked_.empty())
            resend(nowMs);
        else
            flush(nowMs);
        version_++;
    }
    sealIfDue(nowMs);
    for (auto& entry : stations_)
    {
        if (entry.second.askAtMs != 0 && nowMs >= entry.second.askAtMs) ask(entry.first, entry.second, nowMs);
    }
    prune(nowMs);
    forget(nowMs);
}

void Seals::prune(uint64_t nowMs)
{
    while (!seals_.empty())
    {
        const OurSeal& s = seals_.front();
        bool old = s.sent && nowMs >= s.sentMs + MAX_OPEN_MS && (s.keepUntilMs == 0 || nowMs >= s.keepUntilMs);
        if (!old && seals_.size() <= MAX_SEALS_KEPT) break;
        for (uint32_t seq = s.first; seq < s.first + s.count; seq++) sealedItems_.erase(seq);
        asked_.erase(s.number);
        seals_.pop_front();
        version_++;
    }
}

//-------------------------------------------------------------------------
// Theirs
//-------------------------------------------------------------------------

std::vector<Seals::Lack> Seals::lacks(const std::string& call, const Station& station, uint64_t nowMs) const
{
    std::vector<Lack> out;
    std::set<uint16_t> listed;
    for (const auto& entry : station.seals)
    {
        const HeardSeal& h = entry.second;
        if (h.givenUp || nowMs >= h.heardMs + MAX_OPEN_MS) continue;
        // A station that heard none of this over (no item, no piece of a
        // file), and no seal of its before, has just tuned in: it doesn't
        // ask for the over it missed.
        bool following = false;
        for (size_t i = 0; i < h.seal.items.size() && !following; i++)
        {
            uint32_t seq = h.seal.firstItem + (uint32_t)i;
            auto held = station.items.find(seq);
            following = (held != station.items.end() && held->second.digest == h.seal.items[i])
                        || station.piecesOf.count(seq) != 0;
        }
        for (const auto& other : station.seals)
        {
            if (other.first < h.seal.number && nowMs < other.second.heardMs + MAX_OPEN_MS) following = true;
        }
        if (!following) continue;

        Lack lack;
        lack.seal = h.seal.number;
        lack.sealDigest = h.digest;
        lack.items.assign(h.seal.items.size(), false);
        for (size_t i = 0; i < h.seal.items.size(); i++)
        {
            const protocol::Digest& digest = h.seal.items[i];
            if (zero(digest)) continue;
            uint32_t seq = h.seal.firstItem + (uint32_t)i;
            auto held = station.items.find(seq);
            if (held == station.items.end() || held->second.digest != digest)
            {
                lack.items[i] = true;
                continue;
            }
            std::vector<uint16_t> missing;
            if (held->second.type == (uint8_t)protocol::RecordType::FileAnnounce && files_ != nullptr
                && files_->wanted(call, seq, missing) && !missing.empty())
                lack.pieces[seq].insert(missing.begin(), missing.end());
        }
        if (lack.any())
        {
            out.push_back(lack);
            listed.insert(lack.seal);
        }
    }

    // A seal missed: one heard names it, and we were hearing this station
    // before it (items from before the later seal, or older seals).
    for (const auto& entry : station.seals)
    {
        const HeardSeal& h = entry.second;
        if (zero(h.seal.previous) || nowMs >= h.heardMs + MAX_OPEN_MS) continue;
        uint16_t number = before(h.seal.number);
        auto prev = station.seals.find(number);
        if (prev != station.seals.end() && prev->second.digest == h.seal.previous) continue;
        if (listed.count(number) != 0) continue;
        auto asks = station.sealAsks.find(number);
        if (asks != station.sealAsks.end() && asks->second >= MAX_ASKS) continue;
        bool following = false;
        for (const auto& item : station.items)
        {
            if (item.first < h.seal.firstItem && nowMs < item.second.heardMs + MAX_OPEN_MS) following = true;
        }
        for (const auto& piece : station.piecesOf)
        {
            if (piece.first < h.seal.firstItem && nowMs < piece.second + MAX_OPEN_MS) following = true;
        }
        for (const auto& other : station.seals)
        {
            if (other.first < number && nowMs < other.second.heardMs + MAX_OPEN_MS) following = true;
        }
        if (!following) continue;
        Lack lack;
        lack.seal = number;
        lack.wantSeal = true;
        out.push_back(lack);
        listed.insert(number);
    }
    return out;
}

void Seals::armAsk(const std::string& call, Station& station, uint64_t spreadMs, uint64_t nowMs)
{
    if (station.askAtMs != 0) return;
    if (lacks(call, station, nowMs).empty()) return;
    std::uniform_int_distribution<uint64_t> wait(0, spreadMs);
    station.askAtMs = nowMs + wait(random_);
    station.covered.clear();
    version_++;
}

void Seals::heardSeal(const std::string& call, const protocol::Record& record, uint64_t nowMs)
{
    const protocol::Seal* seal = record.as<protocol::Seal>();
    Station& station = stations_[call];
    protocol::Digest digest = protocol::recordDigest(record);
    HeardSeal& h = station.seals[seal->number];
    if (h.digest != digest)
    {
        h = HeardSeal();
        h.seal = *seal;
        h.digest = digest;
        station.sealAsks.erase(seal->number);
    }
    h.heardMs = nowMs;

    // The window opens as the seal ends: ask at a random time within it,
    // leaving room for our request's own airtime and the turn round.
    link::ModeInfo info = modeInfo(mode_);
    uint64_t window = listeningWindowMs(info);
    uint64_t own = (uint64_t)(oneCodewordSeconds(info) * 1000.0) + 2000;
    uint64_t spread = window > own + 1000 ? window - own : 1000;
    armAsk(call, station, spread, nowMs);
    version_++;
}

void Seals::heardRequest(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs)
{
    (void)from;
    (void)nowMs;
    auto found = stations_.find(request.to);
    if (found == stations_.end() || found->second.askAtMs == 0) return;
    Lack& c = found->second.covered[request.seal];
    if (c.sealDigest != request.sealDigest)
    {
        c = Lack();
        c.seal = request.seal;
        c.sealDigest = request.sealDigest;
    }
    c.wantSeal = c.wantSeal || request.wantSeal || zero(request.sealDigest);
    if (c.items.size() < request.missingItems.size()) c.items.resize(request.missingItems.size(), false);
    for (size_t i = 0; i < request.missingItems.size(); i++)
    {
        if (request.missingItems[i]) c.items[i] = true;
    }
    for (const auto& file : request.files) c.pieces[file.first].insert(file.second.begin(), file.second.end());
}

void Seals::ask(const std::string& call, Station& station, uint64_t nowMs)
{
    station.askAtMs = 0;
    std::vector<protocol::Record> records;
    for (Lack lack : lacks(call, station, nowMs))
    {
        // What another station has asked for already is coming anyway.
        auto c = station.covered.find(lack.seal);
        if (c != station.covered.end())
        {
            const Lack& other = c->second;
            bool same = other.sealDigest == lack.sealDigest || zero(other.sealDigest);
            if (same)
            {
                if (other.wantSeal) lack.wantSeal = false;
                for (size_t i = 0; i < lack.items.size() && i < other.items.size(); i++)
                {
                    if (other.items[i]) lack.items[i] = false;
                }
                for (auto file = lack.pieces.begin(); file != lack.pieces.end();)
                {
                    auto theirs = other.pieces.find(file->first);
                    if (theirs != other.pieces.end())
                    {
                        for (uint16_t p : theirs->second) file->second.erase(p);
                    }
                    file = file->second.empty() ? lack.pieces.erase(file) : std::next(file);
                }
            }
        }
        if (!lack.any())
        {
            requestsHeld_++;
            continue;
        }

        // Asking again and again for nothing: give up on this seal.
        auto heard = station.seals.find(lack.seal);
        bool heardIt = heard != station.seals.end() && heard->second.digest == lack.sealDigest && !zero(lack.sealDigest);
        if (heardIt)
        {
            HeardSeal& h = heard->second;
            if (h.asks > 0 && lack.size() < h.lackWhenAsked) h.asks = 0;
            if (h.asks >= MAX_ASKS)
            {
                h.givenUp = true;
                continue;
            }
            h.asks++;
            h.lackWhenAsked = lack.size();
        }
        else
        {
            station.sealAsks[lack.seal]++;
        }

        protocol::RepairRequest request;
        request.to = call;
        request.seal = lack.seal;
        request.sealDigest = lack.sealDigest;
        request.wantSeal = lack.wantSeal;
        request.missingItems = lack.items;
        for (const auto& file : lack.pieces)
            request.files.push_back({file.first, std::vector<uint16_t>(file.second.begin(), file.second.end())});
        protocol::Record record = chat_.record(request, seconds(nowMs));
        // One frame: a long list of pieces is cut, the lowest kept; the
        // rest is asked for next round.
        for (;;)
        {
            std::vector<uint8_t> frame = protocol::encodeFrame({record});
            if (!frame.empty() && frame.size() <= protocol::MAX_BROADCAST_FRAME) break;
            auto* r = record.as<protocol::RepairRequest>();
            auto longest = std::max_element(r->files.begin(), r->files.end(), [](const auto& a, const auto& b) {
                return a.second.size() < b.second.size();
            });
            if (longest == r->files.end()) break;
            if (longest->second.size() <= 1)
                r->files.erase(longest);
            else
                longest->second.resize(longest->second.size() / 2);
        }
        records.push_back(std::move(record));
        requestsSent_++;
    }
    station.covered.clear();
    if (records.empty()) return;
    link::ModeInfo info = modeInfo(mode_);
    for (const auto& burst : packBursts(packFrames(records), info)) link_.broadcast(burst, mode_, nowMs, "repair request");
    version_++;
}

void Seals::askSoon(const std::string& call, uint64_t nowMs)
{
    auto found = stations_.find(protocol::normalCallsign(call));
    if (found == stations_.end()) return;
    armAsk(found->first, found->second, LATE_SPREAD_MS, nowMs);
}

bool Seals::askAgain(const std::string& call, uint32_t seq, uint64_t nowMs)
{
    auto found = stations_.find(protocol::normalCallsign(call));
    if (found == stations_.end()) return false;
    bool listed = false;
    for (auto& entry : found->second.seals)
    {
        HeardSeal& h = entry.second;
        if (!covers(h.seal, seq) || nowMs >= h.heardMs + MAX_OPEN_MS) continue;
        h.givenUp = false;
        h.asks = 0;
        h.lackWhenAsked = 0;
        listed = true;
    }
    if (listed) armAsk(found->first, found->second, LATE_SPREAD_MS, nowMs);
    version_++;
    return listed;
}

bool Seals::gaveUp(const std::string& call, uint32_t seq) const
{
    auto found = stations_.find(call);
    if (found == stations_.end()) return false;
    for (const auto& entry : found->second.seals)
    {
        if (covers(entry.second.seal, seq) && entry.second.givenUp) return true;
    }
    return false;
}

bool Seals::sealHeard(const std::string& call, uint32_t seq) const
{
    auto found = stations_.find(call);
    if (found == stations_.end()) return false;
    for (const auto& entry : found->second.seals)
    {
        if (covers(entry.second.seal, seq)) return true;
    }
    return false;
}

bool Seals::asking(const std::string& call) const
{
    auto found = stations_.find(call);
    return found != stations_.end() && found->second.askAtMs != 0;
}

void Seals::forget(uint64_t nowMs)
{
    static_assert(FORGET_EVERY_MS > 0, "");
    if (nowMs < forgotMs_ + FORGET_EVERY_MS) return;
    forgotMs_ = nowMs;
    for (auto station = stations_.begin(); station != stations_.end();)
    {
        auto& items = station->second.items;
        for (auto it = items.begin(); it != items.end();)
            it = nowMs >= it->second.heardMs + MAX_OPEN_MS ? items.erase(it) : std::next(it);
        auto& seals = station->second.seals;
        for (auto it = seals.begin(); it != seals.end();)
            it = nowMs >= it->second.heardMs + MAX_OPEN_MS ? seals.erase(it) : std::next(it);
        auto& pieces = station->second.piecesOf;
        for (auto it = pieces.begin(); it != pieces.end();)
            it = nowMs >= it->second + MAX_OPEN_MS ? pieces.erase(it) : std::next(it);
        bool empty = items.empty() && seals.empty() && pieces.empty() && station->second.askAtMs == 0;
        station = empty ? stations_.erase(station) : std::next(station);
    }
}

} // namespace broadcast
} // namespace ragtime
