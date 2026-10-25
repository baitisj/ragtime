#include "ragtime/broadcast/WatchList.h"

#include <algorithm>
#include <utility>

#include "ragtime/protocol/Fields.h"

namespace ragtime
{
namespace broadcast
{

void WatchList::setMyCallsign(const std::string& callsign)
{
    me_ = protocol::normalCallsign(callsign);
    if (watches_.erase(me_) > 0) version_++;
}

void WatchList::setPartner(const std::string& call)
{
    partner_ = call;
}

void WatchList::setCalls(const std::vector<std::string>& calls)
{
    for (auto& [call, w] : watches_) w.bell = false;
    for (const std::string& c : calls)
    {
        std::string call = protocol::normalCallsign(c);
        if (!call.empty() && call != me_) watches_[call].bell = true;
    }
    for (auto it = watches_.begin(); it != watches_.end();)
    {
        if (!it->second.bell && !it->second.free)
            it = watches_.erase(it);
        else
            ++it;
    }
    version_++;
}

std::vector<std::string> WatchList::calls() const
{
    std::vector<std::string> out;
    for (const auto& [call, w] : watches_)
    {
        if (w.bell) out.push_back(call);
    }
    return out;
}

bool WatchList::watch(const std::string& c)
{
    std::string call = protocol::normalCallsign(c);
    if (call.empty() || call == me_) return false;
    Watch& w = watches_[call];
    if (!w.bell)
    {
        w.bell = true;
        version_++;
    }
    return true;
}

void WatchList::unwatch(const std::string& c)
{
    auto it = watches_.find(protocol::normalCallsign(c));
    if (it == watches_.end() || !it->second.bell) return;
    it->second.bell = false;
    if (!it->second.free) watches_.erase(it);
    version_++;
}

bool WatchList::watching(const std::string& call) const
{
    auto it = watches_.find(call);
    return it != watches_.end() && it->second.bell;
}

bool WatchList::tellWhenFree(const std::string& c)
{
    std::string call = protocol::normalCallsign(c);
    if (call.empty() || call == me_) return false;
    Watch& w = watches_[call];
    if (!w.free)
    {
        w.free = true;
        version_++;
    }
    return true;
}

void WatchList::cancelWhenFree(const std::string& c)
{
    auto it = watches_.find(protocol::normalCallsign(c));
    if (it == watches_.end() || !it->second.free) return;
    it->second.free = false;
    if (!it->second.bell) watches_.erase(it);
    version_++;
}

bool WatchList::waitingFree(const std::string& call) const
{
    auto it = watches_.find(call);
    return it != watches_.end() && it->second.free;
}

std::string WatchList::bellTip(const std::string& call) const
{
    auto it = watches_.find(call);
    if (it == watches_.end()) return std::string();
    if (it->second.free && it->second.bell) return "Watched, and waiting for " + call + " to be free";
    if (it->second.free) return "Waiting for " + call + " to be free: Ragtime offers to call at its QRV";
    if (it->second.bell) return "Watched: a notification when " + call + " is heard or sends QRV";
    return std::string();
}

void WatchList::heard(const std::string& call, uint64_t nowMs, std::optional<double> snr)
{
    burst(call, nowMs, snr, false);
}

void WatchList::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const protocol::Status* status = record.as<protocol::Status>();
    burst(record.header.callsign, nowMs, std::nullopt, status != nullptr && status->state == protocol::Status::State::Qrv);
}

void WatchList::burst(const std::string& call, uint64_t nowMs, std::optional<double> snr, bool qrv)
{
    auto it = watches_.find(call);
    if (call.empty() || it == watches_.end()) return;
    Watch& w = it->second;
    bool quiet = !w.heardThisRun || nowMs >= w.lastHeardMs + QUIET_MS;
    w.heardThisRun = true;
    w.lastHeardMs = std::max(w.lastHeardMs, nowMs);
    if (call == partner_) return;

    // A ring not yet taken for this station: this one adds to it.
    auto pending = std::find_if(rings_.begin(), rings_.end(), [&](const Ring& r) { return r.call == call; });
    if (qrv && (w.bell || w.free))
    {
        Ring ring;
        ring.call = call;
        ring.why = Ring::Why::Qrv;
        ring.free = w.free;
        ring.snrDb = snr;
        if (pending != rings_.end())
        {
            if (!ring.snrDb) ring.snrDb = pending->snrDb;
            *pending = ring;
        }
        else
        {
            rings_.push_back(ring);
        }
        if (w.free)
        {
            w.free = false;
            if (!w.bell) watches_.erase(it);
            version_++;
        }
        return;
    }
    if (pending != rings_.end())
    {
        if (!pending->snrDb) pending->snrDb = snr;
        return;
    }
    if (w.bell && quiet)
    {
        Ring ring;
        ring.call = call;
        ring.snrDb = snr;
        rings_.push_back(ring);
    }
}

std::vector<Ring> WatchList::takeRings()
{
    std::vector<Ring> out;
    out.swap(rings_);
    return out;
}

} // namespace broadcast
} // namespace ragtime
