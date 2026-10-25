#include "ragtime/broadcast/Seals.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ragtime/broadcast/Airtime.h"
#include "ragtime/files/Parity.h"
#include "ragtime/gearbox/Submodes.h"

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
// Marks kept once their seals and items are gone, at most, ours and theirs.
constexpr size_t MAX_MARKS = 2000;

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

// S/N reports kept for a station, at most.
constexpr size_t MAX_SNRS = 100;

// ItemParity indexes go round before ITEM_PARITY_BASE + index passes the
// top of 16 bits.
constexpr uint32_t ITEM_PARITY_SPAN = 65536 - protocol::ITEM_PARITY_BASE;

// The airtime records take as frames packed into bursts in a mode.
double airOf(const std::vector<protocol::Record>& records, const link::ModeInfo& info)
{
    double air = 0.0;
    for (const auto& burst : packBursts(packFrames(records), info)) air += burstSeconds(burst, info);
    return air;
}

// A share lost, from symbols that came of those sent, held at 0 to 1.
double lostShare(size_t came, size_t sent)
{
    if (sent == 0) return 0.0;
    return std::min(1.0, std::max(0.0, 1.0 - (double)came / (double)sent));
}

} // namespace

const char* refusalText(uint8_t reason)
{
    switch ((protocol::RefusalReason)reason)
    {
        case protocol::RefusalReason::StoppedSharing:
            return "stopped sharing it";
        case protocol::RefusalReason::Cancelled:
            return "cancelled it";
        case protocol::RefusalReason::NotHeld:
            return "no longer holds it";
        case protocol::RefusalReason::Closed:
            return "closed its repairs";
    }
    return "won't repair it";
}

size_t Seals::Lack::missingItems() const
{
    return (size_t)std::count(items.begin(), items.end(), true);
}

bool Seals::Lack::any() const
{
    return wantSeal || missingItems() != 0 || !files.empty();
}

size_t Seals::Lack::size() const
{
    size_t n = wantSeal ? 1 : 0;
    n += missingItems();
    for (const auto& file : files) n += file.second.need;
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
        refusing_.clear();
        refusedMs_.clear();
        resendBursts_.clear();
        resendLast_ = 0;
        roundBursts_.clear();
        repairAir_.clear();
        gears_.clear();
        sealSoon_ = false;
        settled_.clear();
        roundItems_.clear();
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

double Seals::speedOf(const std::string& mode) const
{
    link::ModeInfo info = modeInfo(mode);
    if (!info.valid()) return 0.0;
    return gearbox::payloadBitsPerSecond(info.bytesPerCodeword, info.maxCodewords, info.secondsAtOne,
                                         info.secondsAtMax);
}

std::string Seals::slower(const std::string& a, const std::string& b) const
{
    if (a == b) return a;
    double sa = speedOf(a), sb = speedOf(b);
    if (sa <= 0.0) return b;
    if (sb <= 0.0) return a;
    return sb < sa ? b : a;
}

std::optional<std::string> Seals::gearFor(std::optional<double> snr) const
{
    if (!snr || ladder_.empty()) return std::nullopt;
    int pick = gearbox::autoGearIndex(ladder_, *snr, REPAIR_MARGIN_DB);
    return ladder_[(size_t)pick].mode;
}

std::string Seals::faster(const std::string& a, const std::string& b) const
{
    if (a == b) return a;
    double sa = speedOf(a), sb = speedOf(b);
    if (sa <= 0.0) return b;
    if (sb <= 0.0) return a;
    return sb > sa ? b : a;
}

std::string Seals::repairMode(std::optional<double> snr, const std::string& first, uint32_t seq) const
{
    if (seq != 0)
    {
        auto own = gears_.find(seq);
        if (own != gears_.end()) return own->second;
    }
    std::optional<std::string> gear = gearFor(snr);
    if (!gear) return first;
    // Never faster than it first went: that gear failed someone already.
    // Empty, it went in the group's mode as it stood.
    const std::string& went = !first.empty() ? first : !link_.groupMode().empty() ? link_.groupMode() : mode_;
    std::string mode = slower(*gear, went);
    // Never slower than the floor, though: but no faster than it went.
    if (!floor_.empty() && speedOf(floor_) > 0.0) mode = faster(mode, slower(floor_, went));
    return mode;
}

void Seals::setRepairGear(uint32_t seq, const std::string& mode)
{
    if (mode.empty())
        gears_.erase(seq);
    else
        gears_[seq] = mode;
    // What the round has queued in the old gear goes back; the round's
    // seals still go, and the requests they bring are answered in this one.
    const OurSeal* s = sealFor(seq);
    if (s != nullptr) takeBackRepairs(s->number);
    version_++;
}

std::string Seals::repairGear(uint32_t seq) const
{
    auto own = gears_.find(seq);
    return own != gears_.end() ? own->second : std::string();
}

double Seals::repairAirtime(uint32_t seq) const
{
    auto air = repairAir_.find(seq);
    return air != repairAir_.end() ? air->second : 0.0;
}

bool Seals::repairCapReached(uint32_t seq) const
{
    return repairCapSeconds_ > 0.0 && repairAirtime(seq) >= repairCapSeconds_;
}

void Seals::takeBackRepairs(uint16_t number)
{
    for (auto it = roundBursts_.begin(); it != roundBursts_.end();)
    {
        const std::set<uint16_t>& under = it->second.seals;
        if (under.size() == 1 && *under.begin() == number && link_.cancel(it->first))
        {
            resendBursts_.erase(it->first);
            it = roundBursts_.erase(it);
            version_++;
        }
        else
        {
            ++it;
        }
    }
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

bool Seals::reopen(uint32_t seq, uint64_t untilMs, uint64_t nowMs)
{
    const OurSeal* found = sealFor(seq);
    if (found == nullptr || !found->sent) return false;
    OurSeal* s = findSeal(found->number);
    bool closed = s->closed || s->capped || !open(*s, nowMs);
    if (!keepOpen(seq, untilMs)) return false;
    if (!closed) return true;
    // Its files' repairs start afresh under the cap, and what we refused
    // may be refused again.
    s->capped = false;
    for (uint32_t i = s->first; i < s->first + s->count; i++) repairAir_.erase(i);
    for (auto it = refusedMs_.begin(); it != refusedMs_.end();)
        it = it->first.first == s->number ? refusedMs_.erase(it) : std::next(it);
    // The seal again: a station that heard us refuse asks once more.
    uint64_t id = link_.broadcast({protocol::encodeFrame({s->record})}, s->mode, nowMs, "seal again");
    if (id != 0 && resendLast_ == 0)
    {
        resendBursts_.insert(id);
        resendLast_ = id;
        lastRoundMode_ = s->mode;
    }
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
    takeBackRepairs(s->number);
    version_++;
    return true;
}

std::optional<uint64_t> Seals::nextSealMs() const
{
    if (callsign_.empty()) return std::nullopt;
    // As sealIfDue reckons it.
    bool anySent = false;
    uint64_t oldestSent = std::numeric_limits<uint64_t>::max();
    for (const OurItem& item : unsealed_)
    {
        if (item.fate == Fate::Going) return std::nullopt;
        if (item.file && files_ != nullptr && files_->streaming(item.seq)) return std::nullopt;
        if (item.fate != Fate::Sent) continue;
        anySent = true;
        oldestSent = std::min(oldestSent, item.sentMs);
    }
    if (!anySent) return std::nullopt;
    if (sealSoon_) return lastItemMs_;
    return std::min(lastItemMs_ + QUIET_MS, oldestSent + SEAL_EVERY_MS);
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

void Seals::onSealOver(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs)
{
    // Seal 0: the asker heard these files and none of the seals over
    // them. Each seal goes again; one we can't send is refused.
    for (const auto& file : request.files)
    {
        uint32_t seq = file.first;
        const OurSeal* found = sealFor(seq);
        if (found == nullptr)
        {
            // Not sealed yet (its first pass is still going, or its seal
            // is about to go): the seal comes anyway. Gone: refused.
            const OurItem* item = findItem(seq);
            if (item == nullptr && !callsign_.empty())
                refuse(0, protocol::RefusalReason::NotHeld, seq, repairMode(request.snr, mode_), nowMs);
            continue;
        }
        OurSeal* s = findSeal(found->number);
        if (!s->sent) continue;
        std::string mode = repairMode(request.snr, s->mode, seq);
        if (!open(*s, nowMs))
        {
            auto reason = !s->closed ? protocol::RefusalReason::Closed
                          : files_ != nullptr && files_->stopped(seq) ? protocol::RefusalReason::Cancelled
                                                                       : protocol::RefusalReason::StoppedSharing;
            // Naming the file: the asker doesn't hold the seal.
            refuse(s->number, reason, seq, mode, nowMs);
            continue;
        }
        Asked& a = asked_[s->number];
        a.sealMode = a.seal ? slower(a.sealMode, mode) : mode;
        a.seal = true;
        s->askers[from] = nowMs;
        s->asks++;
        openWindow(s->mode, nowMs);
        version_++;
    }
}

void Seals::onRequest(const std::string& from, const protocol::RepairRequest& request, uint64_t nowMs)
{
    if (request.seal == 0)
    {
        onSealOver(from, request, nowMs);
        return;
    }
    OurSeal* s = findSeal(request.seal);
    if (s == nullptr)
    {
        // One of ours, gone: pruned, or from before a restart.
        if (ours(request.seal))
            refuse(request.seal, protocol::RefusalReason::NotHeld, 0, repairMode(request.snr, mode_), nowMs);
        return;
    }
    if (!zero(request.sealDigest) && request.sealDigest != s->digest) return;
    // A station that heard the seal says, item by item, what it lacks: the
    // rest it holds.
    const protocol::Seal* listed = s->record.as<protocol::Seal>();
    for (size_t i = 0; listed != nullptr && !zero(request.sealDigest) && i < listed->items.size()
                       && i < request.missingItems.size();
         i++)
    {
        if (zero(listed->items[i])) continue;
        uint32_t seq = s->first + (uint32_t)i;
        bool missing = request.missingItems[i];
        auto served = s->servedTo.find(seq);
        if (served != s->servedTo.end()) served->second.erase(from);
        (missing ? s->wanting : s->holders)[seq].insert(from);
        auto other = (missing ? s->holders : s->wanting).find(seq);
        if (other != (missing ? s->holders : s->wanting).end()) other->second.erase(from);
        version_++;
    }
    if (!s->sent) return;
    // A file under it whose repairs have had the airtime allowed closes it.
    for (uint32_t seq = s->first; seq < s->first + s->count && !s->closed; seq++)
    {
        const OurItem* item = findItem(seq);
        if (item != nullptr && item->file && repairCapReached(seq))
        {
            closeNow(seq);
            s->capped = true;
        }
    }
    if (!open(*s, nowMs))
    {
        // Closed by the operator: the file under it cancelled, or sharing
        // stopped; else out of its sensitivity window, or at the cap.
        auto reason = protocol::RefusalReason::Closed;
        if (s->closed && !s->capped)
        {
            reason = protocol::RefusalReason::StoppedSharing;
            for (uint32_t seq = s->first; seq < s->first + s->count && files_ != nullptr; seq++)
            {
                if (files_->stopped(seq)) reason = protocol::RefusalReason::Cancelled;
            }
        }
        refuse(s->number, reason, 0, repairMode(request.snr, s->mode), nowMs);
        return;
    }

    Asked& a = asked_[request.seal];
    if (request.wantSeal || zero(request.sealDigest))
    {
        std::string mode = repairMode(request.snr, s->mode);
        a.sealMode = a.seal ? slower(a.sealMode, mode) : mode;
        a.seal = true;
    }
    // Items, each in the slowest gear of those who asked for it; and what
    // this asker lacks and wants, to size the parity in each gear.
    std::set<uint32_t> lacked;
    for (size_t i = 0; i < request.missingItems.size() && i < s->count; i++)
    {
        if (!request.missingItems[i]) continue;
        const OurItem* item = findItem(s->first + (uint32_t)i);
        if (item == nullptr || item->fate != Fate::Sent) continue;
        std::string mode = repairMode(request.snr, item->mode, item->seq);
        auto have = a.items.find(item->seq);
        a.items[item->seq] = have == a.items.end() ? mode : slower(have->second, mode);
        lacked.insert(item->seq);
    }
    if (!lacked.empty())
    {
        uint32_t want = std::max<uint32_t>(request.itemsWant, (uint32_t)lacked.size());
        a.itemAskers.push_back({std::move(lacked), want});
    }
    for (const auto& file : request.files)
    {
        uint32_t seq = file.first;
        if (seq < s->first || seq >= s->first + s->count || files_ == nullptr || file.second == 0) continue;
        uint16_t pieces = files_->pieceCount(seq);
        const OurItem* item = findItem(seq);
        std::string mode = repairMode(request.snr, item != nullptr ? item->mode : s->mode, seq);
        if (pieces == 0)
        {
            // Asked for pieces we no longer hold.
            refuse(s->number, files_->stopped(seq) ? protocol::RefusalReason::Cancelled
                                                   : protocol::RefusalReason::NotHeld,
                   seq, mode, nowMs);
            continue;
        }
        // Even a station that lost all of it needs no more than its losses
        // allow for: four times the file (wantFor holds the loss at 75%).
        uint32_t want = std::min<uint32_t>(file.second, 4u * pieces);
        uint32_t& most = a.files[seq][mode];
        most = std::max(most, want);
    }
    if (!a.seal && a.items.empty() && a.files.empty()) asked_.erase(request.seal);
    s->askers[from] = nowMs;
    s->asks++;
    // A request with no window open, from a late joiner: a window now, so
    // other requests can join it.
    openWindow(s->mode, nowMs);
    version_++;
}

bool Seals::ours(uint16_t number) const
{
    if (callsign_.empty() || lastNumber_ == 0 || number == 0) return false;
    // At or before our newest seal, counting round the wrap.
    uint32_t back = lastNumber_ >= number ? (uint32_t)(lastNumber_ - number)
                                          : (uint32_t)lastNumber_ + MAX_SEAL_NUMBER - number;
    return back < MAX_SEAL_NUMBER / 2;
}

void Seals::refuse(uint16_t seal, protocol::RefusalReason reason, uint32_t file, const std::string& mode,
                   uint64_t nowMs)
{
    auto key = std::make_pair(seal, (uint8_t)reason);
    auto waiting = refusing_.find(key);
    if (waiting == refusing_.end())
    {
        // Said already, lately: a station that keeps asking can't keep us
        // talking.
        auto last = refusedMs_.find(key);
        if (last != refusedMs_.end() && nowMs < last->second + REFUSE_EVERY_MS) return;
        waiting = refusing_.emplace(key, Refusing()).first;
        waiting->second.mode = mode;
    }
    Refusing& r = waiting->second;
    r.mode = slower(r.mode, mode);
    if (file == 0)
        r.all = true;
    else
        r.files.insert(file);
    openWindow(mode, nowMs);
    version_++;
}

void Seals::openWindow(const std::string& mode, uint64_t nowMs)
{
    if (windowEndsMs_ == 0 && resendLast_ == 0) windowEndsMs_ = nowMs + listeningWindowMs(modeInfo(mode));
}

void Seals::resend(uint64_t nowMs)
{
    rounds_++;
    // Gear by gear: under each seal, the items asked for (the lines, or
    // parity over them, as many as the neediest asker in that gear wants),
    // then its files' parity pieces, each as many as the neediest asker
    // in that gear wants.
    std::map<std::string, std::vector<protocol::Record>> byMode;
    std::vector<const OurSeal*> seals;
    std::vector<std::string> sealModes;
    roundItems_.clear();
    uint32_t time = seconds(nowMs);
    // The round's own gear is its newest seal's. Parity pieces are made to
    // fit its slow gears' cap, as they're numbered one after another: one
    // made and then cut from the round would look lost to the listener.
    const OurSeal* newestAsked = !asked_.empty() ? findSeal(asked_.rbegin()->first) : nullptr;
    std::string ownPlan = newestAsked != nullptr ? newestAsked->mode : mode_;
    double ownPlanSpeed = speedOf(ownPlan);
    double slowPlanned = 0.0;
    for (const auto& entry : asked_)
    {
        OurSeal* s = findSeal(entry.first);
        if (s == nullptr) continue;
        const Asked& a = entry.second;
        seals.push_back(s);
        if (a.seal) sealModes.push_back(a.sealMode);
        std::map<std::string, std::vector<uint32_t>> gears;
        for (const auto& item : a.items)
        {
            gears[item.second].push_back(item.first);
            roundItems_.insert(item.first);
            // Those who asked for it have it sent again now.
            auto wanting = s->wanting.find(item.first);
            if (wanting == s->wanting.end()) continue;
            s->servedTo[item.first].insert(wanting->second.begin(), wanting->second.end());
            s->wanting.erase(wanting);
        }
        for (const auto& gear : gears)
        {
            // An asker lacking some of these needs as many symbols as it
            // lacks of them, scaled up as it asked for its whole lack.
            std::set<uint32_t> here(gear.second.begin(), gear.second.end());
            uint32_t want = 0;
            for (const auto& asker : a.itemAskers)
            {
                size_t in = 0;
                for (uint32_t seq : asker.first) in += here.count(seq);
                if (in == 0) continue;
                uint64_t scaled = ((uint64_t)asker.second * in + asker.first.size() - 1) / asker.first.size();
                want = std::max(want, (uint32_t)scaled);
            }
            if (want == 0) want = (uint32_t)gear.second.size();
            for (auto& r : itemRepairs(*s, gear.second, want, gear.first, time))
                byMode[gear.first].push_back(std::move(r));
        }
        for (const auto& file : a.files)
        {
            uint32_t seq = file.first;
            auto item = sealedItems_.find(seq);
            uint16_t k = files_ != nullptr ? files_->pieceCount(seq) : 0;
            if (item == sealedItems_.end() || k == 0) continue;
            uint32_t span = 65536u - k;
            for (const auto& gear : file.second)
            {
                link::ModeInfo info = modeInfo(gear.first);
                uint32_t limit = gear.second;
                for (uint32_t i = 0; i < limit; i++)
                {
                    uint16_t index = (uint16_t)(k + item->second.nextParity++ % span);
                    protocol::Record piece;
                    if (!files_->piece(seq, index, time, piece)) break;
                    if (i == 0)
                    {
                        // No more than the round can carry: each parity
                        // piece is the work of the whole file to make.
                        std::vector<protocol::Record> some(MAX_FRAMES_PER_BURST, piece);
                        double each = airOf(some, info) / MAX_FRAMES_PER_BURST;
                        if (each > 0.0) limit = std::min<uint32_t>(limit, (uint32_t)(RESEND_MAX_SECONDS / each) + 1);
                        // And no more than the cap has left for it.
                        if (each > 0.0 && repairCapSeconds_ > 0.0)
                        {
                            double left = std::max(0.0, repairCapSeconds_ - repairAirtime(seq));
                            limit = std::min<uint32_t>(limit, (uint32_t)std::ceil(left / each));
                        }
                        // Nor more than the slow gears' share has left: the
                        // round's first burst always goes.
                        bool slow = gear.first != ownPlan && ownPlanSpeed > 0.0 && speedOf(gear.first) < ownPlanSpeed;
                        if (each > 0.0 && slow)
                        {
                            double left = std::max(0.0, slowResendSeconds_ - slowPlanned);
                            uint32_t fits = (uint32_t)(left / each);
                            if (fits == 0 && slowPlanned == 0.0) fits = 1;
                            limit = std::min(limit, fits);
                            slowPlanned += each * limit;
                        }
                        if (limit == 0) break;
                    }
                    byMode[gear.first].push_back(std::move(piece));
                    paritySent_++;
                }
            }
        }
    }
    asked_.clear();
    std::vector<protocol::Record> refusals;
    std::vector<std::string> refusalModes;
    for (const auto& entry : refusing_)
    {
        protocol::RepairRefused refused;
        refused.seal = entry.first.first;
        refused.reason = entry.first.second;
        if (!entry.second.all) refused.files.assign(entry.second.files.begin(), entry.second.files.end());
        refusals.push_back(chat_.record(refused, time));
        refusalModes.push_back(entry.second.mode);
        refusedMs_[entry.first] = nowMs;
        refusalsSent_++;
    }
    refusing_.clear();
    if (seals.empty() && refusals.empty()) return;
    std::sort(seals.begin(), seals.end(), [](const OurSeal* a, const OurSeal* b) { return a->number < b->number; });

    // Fastest first, so the stations that hear us best are served soonest.
    std::vector<std::string> modes;
    for (const auto& entry : byMode) modes.push_back(entry.first);
    std::stable_sort(modes.begin(), modes.end(),
                     [this](const std::string& a, const std::string& b) { return speedOf(a) > speedOf(b); });

    // Within this round's airtime, and its slow gears' share of it; the
    // rest is asked for again.
    std::string own = !seals.empty() ? seals.back()->mode : mode_;
    double ownSpeed = speedOf(own);
    double air = 0.0, slowAir = 0.0;
    std::string last;
    std::vector<std::pair<std::string, std::vector<std::vector<uint8_t>>>> bursts;
    for (const std::string& mode : modes)
    {
        link::ModeInfo info = modeInfo(mode);
        bool slow = mode != own && ownSpeed > 0.0 && speedOf(mode) < ownSpeed;
        for (auto& burst : packBursts(packFrames(byMode[mode]), info))
        {
            double seconds = burstSeconds(burst, info);
            bool first = bursts.empty();
            if (!first && (air + seconds > RESEND_MAX_SECONDS || (slow && slowAir + seconds > slowResendSeconds_)))
                break;
            air += seconds;
            if (slow) slowAir += seconds;
            last = last.empty() ? mode : slower(last, mode);
            bursts.push_back({mode, std::move(burst)});
        }
    }
    // The seals last, in the slowest gear used, so every asker can copy
    // them: hearing one again opens the next window. Refusals after them.
    std::string end = last.empty() ? own : last;
    for (const std::string& mode : sealModes) end = slower(end, mode);
    for (const std::string& mode : refusalModes) end = slower(end, mode);
    std::vector<std::vector<uint8_t>> closing;
    for (const OurSeal* s : seals) closing.push_back(protocol::encodeFrame({s->record}));
    for (const auto& frame : packFrames(refusals)) closing.push_back(frame);

    size_t repairs = bursts.size();
    for (auto& burst : packBursts(closing, modeInfo(end))) bursts.push_back({end, std::move(burst)});

    roundBursts_.clear();
    for (size_t i = 0; i < bursts.size(); i++)
    {
        const auto& burst = bursts[i];
        uint64_t id = link_.broadcast(burst.second, burst.first, nowMs,
                                      seals.empty() ? "repairs refused" : "repairs");
        if (id == 0) continue;
        resendBursts_.insert(id);
        resendLast_ = id;
        if (i >= repairs) continue;
        // What it carries: repairs under which seals, and each file's
        // share of its airtime, counted to the file once it has gone.
        RoundBurst& round = roundBursts_[id];
        double each = burstSeconds(burst.second, modeInfo(burst.first)) / (double)burst.second.size();
        for (const auto& frame : burst.second)
        {
            std::vector<protocol::Record> records;
            if (!protocol::decodeFrame(frame, records)) continue;
            for (const protocol::Record& r : records)
            {
                const protocol::ItemParity* p = r.as<protocol::ItemParity>();
                const OurSeal* under = p != nullptr ? nullptr : sealFor(r.header.seq);
                if (p != nullptr) round.seals.insert(p->seal);
                else if (under != nullptr) round.seals.insert(under->number);
            }
            if (!records.empty() && records.front().as<protocol::FilePiece>() != nullptr)
                round.fileAir[records.front().header.seq] += each;
        }
    }
    lastRoundMode_ = end;
    version_++;
}

std::vector<protocol::Record> Seals::itemRepairs(OurSeal& s, const std::vector<uint32_t>& items, uint32_t want,
                                                const std::string& mode, uint32_t time)
{
    std::vector<protocol::Record> plain;
    std::vector<std::vector<uint8_t>> bytes;
    size_t longest = 0;
    for (uint32_t seq : items)
    {
        const OurItem* item = findItem(seq);
        if (item == nullptr) continue;
        plain.push_back(item->record);
        bytes.push_back(protocol::encodeRecord(item->record));
        longest = std::max(longest, bytes.back().size());
    }
    if (plain.empty() || longest == 0) return plain;

    // Parity over these items: each is the data symbol named by its place
    // in the seal, its encoded record padded to the longest.
    size_t length = files::parity::symbolLength(longest);
    protocol::ItemParity p;
    p.seal = s.number;
    p.items.assign(s.count, false);
    std::vector<files::parity::Symbol> data;
    for (size_t i = 0; i < items.size() && i < bytes.size(); i++)
    {
        uint32_t place = items[i] - s.first;
        p.items[place] = true;
        data.push_back({(uint16_t)place, &bytes[i]});
    }
    // A long line may leave no room for its parity in a frame: the lines
    // go as they are.
    p.bytes.assign(length, 0);
    std::vector<uint8_t> probe = protocol::encodeFrame({chat_.record(p, time)});
    if (probe.empty() || probe.size() > protocol::MAX_BROADCAST_FRAME) return plain;

    auto parity = [&](uint32_t n) {
        std::vector<protocol::Record> out;
        for (uint32_t i = 0; i < n; i++)
        {
            p.index = (uint16_t)(s.nextParity++ % ITEM_PARITY_SPAN);
            p.bytes = files::parity::parityOf((uint16_t)(protocol::ITEM_PARITY_BASE + p.index), data, length);
            if (p.bytes.empty()) break;
            out.push_back(chat_.record(p, time));
        }
        return out;
    };
    if (want >= plain.size())
    {
        // Each asker lacks some of them and expects to lose more: the
        // lines themselves, and parity after them for what is lost.
        std::vector<protocol::Record> out = plain;
        std::vector<protocol::Record> more = parity(want - (uint32_t)plain.size());
        paritySent_ += more.size();
        out.insert(out.end(), more.begin(), more.end());
        return out;
    }
    // Fewer symbols than lines fill every asker's gaps (each holds what it
    // didn't ask for): parity, unless the lines cost no more airtime.
    uint32_t keep = s.nextParity;
    std::vector<protocol::Record> some = parity(want);
    link::ModeInfo info = modeInfo(mode);
    if (some.size() < want || airOf(some, info) >= airOf(plain, info))
    {
        s.nextParity = keep;
        return plain;
    }
    paritySent_ += some.size();
    return some;
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
    lastRoundMode_ = mode;
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
    if (event.type == Event::Type::HeardFrom && event.snr)
        heardSnr(event.text, *event.snr, nowMs);
    else if (event.type == Event::Type::SessionSnr && event.snr)
        heardSnr(event.peer, *event.snr, nowMs);
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

    auto round = roundBursts_.find(event.id);
    if (round != roundBursts_.end())
    {
        std::map<uint32_t, double> fileAir = round->second.fileAir;
        roundBursts_.erase(round);
        for (const auto& air : fileAir)
        {
            if (!sent) continue;
            repairAir_[air.first] += air.second;
            // Its repairs have had the airtime allowed: closed, the rest of
            // the round's repairs under its seal taken back.
            const OurSeal* under = sealFor(air.first);
            if (under == nullptr || under->closed || !repairCapReached(air.first)) continue;
            closeNow(air.first);
            findSeal(under->number)->capped = true;
        }
        version_++;
    }
    if (resendBursts_.erase(event.id) != 0 && event.id == resendLast_)
    {
        resendLast_ = 0;
        roundItems_.clear();
        // The round's seals went again: listeners ask once more if they
        // still lack anything.
        if (sent)
        {
            windowEndsMs_ = nowMs + listeningWindowMs(modeInfo(lastRoundMode_));
        }
        version_++;
    }
}

void Seals::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (from.empty() || from == callsign_) return;
    // Their own sending, not repair talk about others'.
    if (record.as<protocol::RepairRequest>() == nullptr) stations_[from].lastHeardMs = nowMs;

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
    else if (const protocol::RepairRefused* refused = record.as<protocol::RepairRefused>())
    {
        heardRefusal(from, *refused, nowMs);
    }
    else if (record.as<protocol::ItemParity>() != nullptr)
    {
        heardItemParity(from, record, nowMs);
    }
    else if (protocol::isItem((protocol::RecordType)record.type()))
    {
        Station& station = stations_[from];
        protocol::Digest digest = protocol::recordDigest(record);
        HeardItem& item = station.items[record.header.seq];
        bool fresh = item.digest != digest || item.bytes.empty();
        item.digest = digest;
        item.type = record.type();
        item.heardMs = nowMs;
        item.bytes = protocol::encodeRecord(record);
        // Heard after its seal: a repair, or a late copy.
        for (auto& entry : station.seals)
        {
            const protocol::Seal& seal = entry.second.seal;
            if (!covers(seal, record.header.seq)) continue;
            markTheirs(from, record.header.seq, item.digest, seal.items[record.header.seq - seal.firstItem], nowMs);
            if (fresh) entry.second.came++;
        }
        // One parity symbol fewer needed for the lines it covers.
        std::vector<ParityKey> keys;
        for (const auto& batch : station.parity) keys.push_back(batch.first);
        for (const ParityKey& key : keys) rebuildItems(from, station, key);
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
        if (!asked_.empty() || !refusing_.empty())
            resend(nowMs);
        else
            flush(nowMs);
        version_++;
    }
    sealIfDue(nowMs);
    if (nowMs >= lateCheckMs_ + FORGET_EVERY_MS)
    {
        // Files whose seals were all lost: a request soon.
        lateCheckMs_ = nowMs;
        for (auto& entry : stations_)
        {
            Station& station = entry.second;
            if (station.askAtMs != 0 || (station.piecesOf.empty() && station.items.empty())) continue;
            std::vector<Lack> lacking = lacks(entry.first, station, nowMs);
            // Lacking under a seal, with the station and us quiet since:
            // the seal that ended the last round was lost.
            bool late = std::any_of(lacking.begin(), lacking.end(), [&](const Lack& l) {
                if (l.seal == 0) return true;
                auto heard = station.seals.find(l.seal);
                if (heard == station.seals.end() || heard->second.digest != l.sealDigest) return false;
                uint64_t last = std::max({heard->second.heardMs, heard->second.askedMs, station.lastHeardMs});
                return nowMs >= last + SEAL_LATE_MS;
            });
            if (late) armAsk(entry.first, station, LATE_SPREAD_MS, nowMs);
        }
    }
    for (auto& entry : stations_)
    {
        if (entry.second.askAtMs != 0 && nowMs >= entry.second.askAtMs) ask(entry.first, entry.second, nowMs);
    }
    prune(nowMs);
    forget(nowMs);
}

void Seals::prune(uint64_t nowMs)
{
    for (auto it = refusedMs_.begin(); it != refusedMs_.end();)
        it = nowMs >= it->second + REFUSE_EVERY_MS ? refusedMs_.erase(it) : std::next(it);
    while (!seals_.empty())
    {
        const OurSeal& s = seals_.front();
        bool old = s.sent && nowMs >= s.sentMs + MAX_OPEN_MS && (s.keepUntilMs == 0 || nowMs >= s.keepUntilMs);
        if (!old && seals_.size() <= MAX_SEALS_KEPT) break;
        settle(s);
        for (uint32_t seq = s.first; seq < s.first + s.count; seq++)
        {
            sealedItems_.erase(seq);
            gears_.erase(seq);
            repairAir_.erase(seq);
        }
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
        // Refused: none of it, or none of the files it names.
        auto refused = station.refused.find(h.seal.number);
        if (refused != station.refused.end() && refused->second.all) continue;
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
        size_t listedItems = 0;
        for (size_t i = 0; i < h.seal.items.size(); i++)
        {
            const protocol::Digest& digest = h.seal.items[i];
            if (zero(digest)) continue;
            listedItems++;
            uint32_t seq = h.seal.firstItem + (uint32_t)i;
            if (refused != station.refused.end() && refused->second.files.count(seq) != 0) continue;
            auto held = station.items.find(seq);
            if (held == station.items.end() || held->second.digest != digest)
            {
                lack.items[i] = true;
                continue;
            }
            FileLack file;
            if (held->second.type == (uint8_t)protocol::RecordType::FileAnnounce && files_ != nullptr
                && files_->wanted(call, seq, file.need, file.want) && file.need != 0)
                lack.files[seq] = file;
        }
        // Repair symbols for the items: as many as we lack, and more for
        // the share we expect to lose, the last round's, or else the first
        // pass's.
        size_t missing = lack.missingItems();
        if (missing != 0)
        {
            double loss = h.itemLoss >= 0.0 ? h.itemLoss : lostShare(listedItems - missing, listedItems);
            lack.itemsWant = std::min<uint32_t>(255, files::parity::wantFor((uint32_t)missing, loss));
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
        auto refused = station.refused.find(number);
        if (refused != station.refused.end() && refused->second.all) continue;
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

    // Files heard with no seal over them, long after the last of them
    // came: every copy of the seal was lost. One request under seal 0
    // names them all.
    std::map<uint32_t, uint64_t> heardFiles = station.piecesOf;
    for (const auto& item : station.items)
    {
        if (item.second.type != (uint8_t)protocol::RecordType::FileAnnounce) continue;
        uint64_t& last = heardFiles[item.first];
        last = std::max(last, item.second.heardMs);
    }
    Lack late;
    late.wantSeal = true;
    auto refusedLate = station.refused.find(0);
    for (const auto& file : heardFiles)
    {
        uint32_t seq = file.first;
        if (nowMs < file.second + SEAL_LATE_MS) continue;
        bool sealed = std::any_of(station.seals.begin(), station.seals.end(),
                                  [seq](const auto& seal) { return covers(seal.second.seal, seq); });
        if (sealed) continue;
        if (refusedLate != station.refused.end() && refusedLate->second.files.count(seq) != 0) continue;
        auto asks = station.sealOverAsks.find(seq);
        if (asks != station.sealOverAsks.end()
            && (asks->second.first >= MAX_SEAL_OVER_ASKS || nowMs < asks->second.second + SEAL_LATE_MS))
            continue;
        // Its announce held: only while the operator receives it. Not
        // held, the announce is lost too, and the seal brings it.
        auto announce = station.items.find(seq);
        uint32_t need = 0, want = 0;
        bool announced = announce != station.items.end()
                         && announce->second.type == (uint8_t)protocol::RecordType::FileAnnounce;
        if (announced && (files_ == nullptr || !files_->wanted(call, seq, need, want))) continue;
        late.files[seq] = FileLack();
    }
    if (!late.files.empty()) out.push_back(late);
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
    // Sent again after a refusal of all of it (the operator kept it open
    // after all): it is answered again.
    auto refused = station.refused.find(seal->number);
    if (refused != station.refused.end() && refused->second.all && nowMs > refused->second.heardMs)
        station.refused.erase(refused);
    for (size_t i = 0; i < seal->items.size(); i++)
    {
        auto held = station.items.find(seal->firstItem + (uint32_t)i);
        if (held != station.items.end())
            markTheirs(call, held->first, held->second.digest, seal->items[i], nowMs);
    }

    // Parity heard before its seal can be used now.
    std::vector<ParityKey> keys;
    for (const auto& batch : station.parity)
    {
        if (std::get<0>(batch.first) == seal->number) keys.push_back(batch.first);
    }
    for (const ParityKey& key : keys) rebuildItems(call, station, key);

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
    auto found = stations_.find(request.to);
    if (found == stations_.end() || found->second.askAtMs == 0) return;
    // It speaks for us only if its asker's gear is no faster than ours:
    // else what it asks for comes again in a gear we may not copy. An S/N
    // not given (0.1, or no figure) counts as the gear in use.
    std::string ourGear = gearFor(snrOf(request.to, nowMs)).value_or(mode_);
    std::string theirGear = gearFor(request.snr).value_or(mode_);
    if (slower(ourGear, theirGear) != theirGear) return;
    Lack c;
    c.seal = request.seal;
    c.sealDigest = request.sealDigest;
    c.wantSeal = request.wantSeal || zero(request.sealDigest);
    c.items = request.missingItems;
    c.itemsWant = request.itemsWant;
    for (const auto& file : request.files) c.files[file.first].want = file.second;
    found->second.covered[request.seal].push_back(std::move(c));
}

void Seals::ask(const std::string& call, Station& station, uint64_t nowMs)
{
    station.askAtMs = 0;
    // What came of the symbols last asked for under each seal says what
    // share of a round we lose.
    for (auto& entry : station.seals)
    {
        HeardSeal& h = entry.second;
        if (h.itemsAsked != 0 && h.came != 0) h.itemLoss = lostShare(h.came, h.itemsAsked);
        h.itemsAsked = 0;
        h.came = 0;
    }
    std::vector<protocol::Record> records;
    for (Lack lack : lacks(call, station, nowMs))
    {
        // What another station has asked for already is coming anyway: the
        // seal; our items, if one request lacks all of them and wants as
        // many symbols; a file, if one wants as many of its pieces.
        uint32_t itemsComing = 0;
        std::map<uint32_t, uint32_t> filesComing;
        auto c = station.covered.find(lack.seal);
        if (c != station.covered.end())
        {
            for (const Lack& other : c->second)
            {
                if (other.sealDigest != lack.sealDigest && !zero(other.sealDigest)) continue;
                if (other.wantSeal) lack.wantSeal = false;
                bool all = lack.missingItems() != 0 && other.itemsWant >= lack.itemsWant;
                for (size_t i = 0; i < lack.items.size() && all; i++)
                {
                    if (lack.items[i] && (i >= other.items.size() || !other.items[i])) all = false;
                }
                if (all)
                {
                    itemsComing = lack.itemsWant;
                    std::fill(lack.items.begin(), lack.items.end(), false);
                    lack.itemsWant = 0;
                }
                for (auto file = lack.files.begin(); file != lack.files.end();)
                {
                    auto theirs = other.files.find(file->first);
                    bool coming = theirs != other.files.end() && theirs->second.want >= file->second.want;
                    if (coming) filesComing[file->first] = file->second.want;
                    file = coming ? lack.files.erase(file) : std::next(file);
                }
            }
        }
        // Asking for the seal over files: once a SEAL_LATE_MS, at most
        // MAX_SEAL_OVER_ASKS times, whoever asks.
        if (lack.seal == 0)
        {
            for (const auto& file : filesComing) station.sealOverAsks[file.first].second = nowMs;
            for (const auto& file : lack.files)
            {
                auto& asks = station.sealOverAsks[file.first];
                asks.first++;
                asks.second = nowMs;
            }
        }
        auto heard = station.seals.find(lack.seal);
        bool heardIt = heard != station.seals.end() && heard->second.digest == lack.sealDigest && !zero(lack.sealDigest);
        if (heardIt)
        {
            heard->second.itemsAsked = itemsComing;
            heard->second.askedMs = nowMs;
        }
        for (const auto& file : filesComing)
        {
            if (files_ != nullptr && lack.seal != 0) files_->asked(call, file.first, file.second);
        }
        if (!lack.any())
        {
            requestsHeld_++;
            continue;
        }

        // Asking again and again for nothing: give up on this seal.
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
            if (lack.itemsWant != 0) h.itemsAsked = lack.itemsWant;
        }
        else if (lack.seal != 0)
        {
            station.sealAsks[lack.seal]++;
        }

        protocol::RepairRequest request;
        request.to = call;
        request.seal = lack.seal;
        request.sealDigest = lack.sealDigest;
        request.wantSeal = lack.wantSeal;
        request.snr = snrOf(call, nowMs);
        request.missingItems = lack.items;
        request.itemsWant = (uint8_t)std::min<uint32_t>(255, lack.itemsWant);
        for (const auto& file : lack.files)
            request.files.push_back({file.first, (uint16_t)std::min<uint32_t>(65535, file.second.want)});
        protocol::Record record = chat_.record(request, seconds(nowMs));
        // One frame: files past what fits are asked for next round.
        for (;;)
        {
            std::vector<uint8_t> frame = protocol::encodeFrame({record});
            if (!frame.empty() && frame.size() <= protocol::MAX_BROADCAST_FRAME) break;
            auto* r = record.as<protocol::RepairRequest>();
            if (r->files.empty()) break;
            r->files.pop_back();
        }
        if (files_ != nullptr && lack.seal != 0)
        {
            for (const auto& file : record.as<protocol::RepairRequest>()->files)
                files_->asked(call, file.first, file.second);
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

void Seals::heardItemParity(const std::string& call, const protocol::Record& record, uint64_t nowMs)
{
    const protocol::ItemParity* p = record.as<protocol::ItemParity>();
    size_t length = p->bytes.size();
    if (length == 0 || length % 2 != 0 || (uint32_t)p->index >= ITEM_PARITY_SPAN) return;
    Station& station = stations_[call];
    ParityKey key{p->seal, p->items, length};
    ParityBatch& batch = station.parity[key];
    bool fresh = batch.symbols.emplace(p->index, p->bytes).second;
    batch.heardMs = nowMs;
    auto heard = station.seals.find(p->seal);
    if (fresh && heard != station.seals.end()) heard->second.came++;
    rebuildItems(call, station, key);
}

void Seals::rebuildItems(const std::string& call, Station& station, const ParityKey& key)
{
    auto found = station.parity.find(key);
    if (found == station.parity.end()) return;
    // Its seal says which numbers the places are and what each must be.
    auto heard = station.seals.find(std::get<0>(key));
    if (heard == station.seals.end()) return;
    const protocol::Seal& seal = heard->second.seal;
    const std::vector<bool>& covered = std::get<1>(key);
    size_t length = std::get<2>(key);
    if (covered.size() != seal.items.size())
    {
        station.parity.erase(found);
        return;
    }
    std::vector<files::parity::Symbol> known;
    std::vector<uint16_t> missing;
    for (size_t i = 0; i < covered.size(); i++)
    {
        if (!covered[i]) continue;
        auto held = station.items.find(seal.firstItem + (uint32_t)i);
        if (held != station.items.end() && held->second.digest == seal.items[i] && !held->second.bytes.empty())
        {
            // Coded otherwise than the sender coded it: no use.
            if (held->second.bytes.size() > length)
            {
                station.parity.erase(found);
                return;
            }
            known.push_back({(uint16_t)i, &held->second.bytes});
        }
        else
        {
            missing.push_back((uint16_t)i);
        }
    }
    const ParityBatch& batch = found->second;
    if (missing.empty())
    {
        station.parity.erase(found);
        return;
    }
    if (batch.symbols.size() < missing.size()) return;
    std::vector<files::parity::Symbol> parity;
    for (const auto& symbol : batch.symbols)
    {
        if (parity.size() == missing.size()) break;
        parity.push_back({(uint16_t)(protocol::ITEM_PARITY_BASE + symbol.first), &symbol.second});
    }
    std::vector<std::vector<uint8_t>> out;
    bool ok = files::parity::rebuild(known, parity, missing, length, out);
    uint64_t heardMs = batch.heardMs;
    station.parity.erase(found);
    if (!ok) return;
    for (size_t i = 0; i < missing.size(); i++)
    {
        // Each must decode as the item the seal lists at its place.
        uint32_t seq = seal.firstItem + missing[i];
        protocol::Record item;
        const uint8_t* at = out[i].data();
        if (protocol::decodeRecord(at, out[i].data() + out[i].size(), item) != protocol::DecodeResult::Ok) continue;
        if (item.header.callsign != call || item.header.seq != seq || !protocol::isItem((protocol::RecordType)item.type()))
            continue;
        protocol::Digest digest = protocol::recordDigest(item);
        if (digest != seal.items[missing[i]]) continue;
        HeardItem& held = station.items[seq];
        held.digest = digest;
        held.type = item.type();
        held.heardMs = heardMs;
        held.bytes = protocol::encodeRecord(item);
        rebuilt_.push_back(std::move(item));
        itemsRebuilt_++;
        version_++;
    }
}

std::vector<protocol::Record> Seals::takeRebuilt()
{
    std::vector<protocol::Record> out;
    out.swap(rebuilt_);
    return out;
}

void Seals::askSoon(const std::string& call, uint64_t nowMs)
{
    auto found = stations_.find(protocol::normalCallsign(call));
    if (found == stations_.end()) return;
    armAsk(found->first, found->second, LATE_SPREAD_MS, nowMs);
}

//-------------------------------------------------------------------------
// The seal badge
//-------------------------------------------------------------------------

Seals::ItemMark Seals::ourMark(uint32_t seq) const
{
    ItemMark m;
    for (const OurItem& item : unsealed_)
    {
        if (item.seq != seq) continue;
        if (item.fate != Fate::Unsent) m.mark = Mark::Awaiting;
        return m;
    }
    const OurSeal* s = sealFor(seq);
    if (s == nullptr)
    {
        auto settled = settled_.find(seq);
        return settled != settled_.end() ? settled->second : m;
    }
    const protocol::Seal* listed = s->record.as<protocol::Seal>();
    if (listed == nullptr || seq - s->first >= listed->items.size() || zero(listed->items[seq - s->first])) return m;
    if (!s->sent)
    {
        m.mark = Mark::Awaiting;
        return m;
    }
    m.sealedMs = s->sentMs;
    auto holders = s->holders.find(seq);
    if (holders != s->holders.end()) m.holders.assign(holders->second.begin(), holders->second.end());
    auto wanting = s->wanting.find(seq);
    if (wanting != s->wanting.end()) m.wanting.assign(wanting->second.begin(), wanting->second.end());
    auto served = s->servedTo.find(seq);
    if (served != s->servedTo.end()) m.sentAgainFor.assign(served->second.begin(), served->second.end());
    if (!m.wanting.empty() || roundItems_.count(seq) != 0)
        m.mark = Mark::Repairing;
    else
        m.mark = m.holders.empty() ? Mark::Sealed : Mark::Held;
    return m;
}

Seals::ItemMark Seals::theirMark(const std::string& call, uint32_t seq) const
{
    auto found = theirMarks_.find({call, seq});
    return found != theirMarks_.end() ? found->second : ItemMark();
}

void Seals::settle(const OurSeal& seal)
{
    if (!seal.sent) return;
    for (uint32_t seq = seal.first; seq < seal.first + seal.count; seq++)
    {
        ItemMark m = ourMark(seq);
        if (m.mark == Mark::None) continue;
        // Its repairs are over: what was asked for went again.
        m.wanting.clear();
        m.mark = m.holders.empty() ? Mark::Sealed : Mark::Held;
        settled_[seq] = m;
    }
    while (settled_.size() > MAX_MARKS) settled_.erase(settled_.begin());
}

void Seals::markTheirs(const std::string& call, uint32_t seq, const protocol::Digest& held,
                       const protocol::Digest& sealed, uint64_t nowMs)
{
    if (zero(sealed)) return;
    auto key = std::make_pair(call, seq);
    auto found = theirMarks_.find(key);
    if (found == theirMarks_.end())
    {
        found = theirMarks_.emplace(key, ItemMark()).first;
        theirOrder_.push_back(key);
    }
    found->second.mark = held == sealed ? Mark::Sealed : Mark::Mismatch;
    found->second.sealedMs = nowMs;
    while (theirOrder_.size() > MAX_MARKS)
    {
        theirMarks_.erase(theirOrder_.front());
        theirOrder_.pop_front();
    }
    version_++;
}

bool Seals::askAgain(const std::string& call, uint32_t seq, uint64_t nowMs)
{
    auto found = stations_.find(protocol::normalCallsign(call));
    if (found == stations_.end()) return false;
    // Refused: asking again would only be refused again.
    if (refusal(found->first, seq)) return false;
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

std::vector<KeptSeal> Seals::sealsOver(const std::string& call, uint32_t seq) const
{
    std::vector<KeptSeal> out;
    auto found = stations_.find(call);
    if (found == stations_.end()) return out;
    for (const auto& entry : found->second.seals)
    {
        if (covers(entry.second.seal, seq)) out.push_back({entry.second.seal, entry.second.digest, entry.second.heardMs});
    }
    return out;
}

void Seals::restoreHeard(const protocol::Record& announce, const std::vector<KeptSeal>& seals, uint64_t lastMs,
                         uint64_t nowMs)
{
    const std::string& call = announce.header.callsign;
    if (call.empty() || call == callsign_) return;
    Station& station = stations_[call];
    // Held again as of now, from disk: a seal heard over it later doesn't
    // send us asking for its announce.
    HeardItem& item = station.items[announce.header.seq];
    item.digest = protocol::recordDigest(announce);
    item.type = announce.type();
    item.heardMs = nowMs;
    item.bytes = protocol::encodeRecord(announce);
    station.piecesOf[announce.header.seq] = lastMs;
    for (const KeptSeal& s : seals)
    {
        if (nowMs >= s.heardMs + MAX_OPEN_MS) continue;
        HeardSeal& h = station.seals[s.seal.number];
        if (h.digest == s.digest) continue;
        h = HeardSeal();
        h.seal = s.seal;
        h.digest = s.digest;
        h.heardMs = s.heardMs;
    }
    armAsk(call, station, LATE_SPREAD_MS, nowMs);
    version_++;
}

bool Seals::asking(const std::string& call) const
{
    auto found = stations_.find(call);
    return found != stations_.end() && found->second.askAtMs != 0;
}

void Seals::heardSnr(const std::string& call, double dB, uint64_t nowMs)
{
    std::string normal = protocol::normalCallsign(call);
    if (normal.empty() || normal == callsign_) return;
    auto& snrs = stations_[normal].snrs;
    snrs.push_back({nowMs, dB});
    while (snrs.size() > MAX_SNRS || (!snrs.empty() && nowMs >= snrs.front().first + SNR_WINDOW_MS))
        snrs.pop_front();
}

std::optional<double> Seals::snrOf(const std::string& call, uint64_t nowMs) const
{
    auto found = stations_.find(protocol::normalCallsign(call));
    if (found == stations_.end()) return std::nullopt;
    // The mean of the last SNR_BURSTS in the window, newest first.
    const auto& snrs = found->second.snrs;
    double sum = 0.0;
    size_t n = 0;
    for (auto it = snrs.rbegin(); it != snrs.rend() && n < SNR_BURSTS; ++it)
    {
        if (nowMs >= it->first + SNR_WINDOW_MS) break;
        sum += it->second;
        n++;
    }
    if (n == 0) return std::nullopt;
    return sum / (double)n;
}

void Seals::heardRefusal(const std::string& call, const protocol::RepairRefused& refused, uint64_t nowMs)
{
    Station& station = stations_[call];
    Refused& r = station.refused[refused.seal];
    r.reason = refused.reason;
    r.heardMs = nowMs;
    if (refused.files.empty())
        r.all = true;
    else
        r.files.insert(refused.files.begin(), refused.files.end());
    refusalsHeard_++;
    version_++;
}

std::optional<uint8_t> Seals::refusal(const std::string& call, uint32_t seq) const
{
    auto found = stations_.find(call);
    if (found == stations_.end()) return std::nullopt;
    const Station& station = found->second;
    for (const auto& entry : station.refused)
    {
        const Refused& r = entry.second;
        if (r.files.count(seq) != 0) return r.reason;
        auto seal = station.seals.find(entry.first);
        if (r.all && seal != station.seals.end() && covers(seal->second.seal, seq)) return r.reason;
    }
    return std::nullopt;
}

void Seals::forget(uint64_t nowMs)
{
    static_assert(FORGET_EVERY_MS > 0, "");
    if (nowMs < forgotMs_ + FORGET_EVERY_MS) return;
    forgotMs_ = nowMs;
    for (auto station = stations_.begin(); station != stations_.end();)
    {
        auto& seals = station->second.seals;
        for (auto it = seals.begin(); it != seals.end();)
            it = nowMs >= it->second.heardMs + MAX_OPEN_MS ? seals.erase(it) : std::next(it);
        // An item is kept while a seal over it is, or we would seem to
        // lack it.
        auto& items = station->second.items;
        for (auto it = items.begin(); it != items.end();)
        {
            uint32_t seq = it->first;
            bool sealed = std::any_of(seals.begin(), seals.end(),
                                      [seq](const auto& seal) { return covers(seal.second.seal, seq); });
            it = !sealed && nowMs >= it->second.heardMs + MAX_OPEN_MS ? items.erase(it) : std::next(it);
        }
        auto& pieces = station->second.piecesOf;
        for (auto it = pieces.begin(); it != pieces.end();)
            it = nowMs >= it->second + MAX_OPEN_MS ? pieces.erase(it) : std::next(it);
        auto& snrs = station->second.snrs;
        while (!snrs.empty() && nowMs >= snrs.front().first + SNR_WINDOW_MS) snrs.pop_front();
        auto& refused = station->second.refused;
        for (auto it = refused.begin(); it != refused.end();)
            it = nowMs >= it->second.heardMs + MAX_OPEN_MS ? refused.erase(it) : std::next(it);
        auto& sealOver = station->second.sealOverAsks;
        for (auto it = sealOver.begin(); it != sealOver.end();)
            it = nowMs >= it->second.second + MAX_OPEN_MS ? sealOver.erase(it) : std::next(it);
        auto& parity = station->second.parity;
        for (auto it = parity.begin(); it != parity.end();)
            it = nowMs >= it->second.heardMs + MAX_OPEN_MS ? parity.erase(it) : std::next(it);
        bool empty = items.empty() && seals.empty() && pieces.empty() && snrs.empty() && refused.empty()
                     && parity.empty() && station->second.askAtMs == 0;
        station = empty ? stations_.erase(station) : std::next(station);
    }
}

} // namespace broadcast
} // namespace ragtime
