// The watch list: a bell on any callsign, heard before or not, that rings
// when the station is heard on D2G or sends QRV, for skeds across time
// zones. "Tell me when free" (docs/PLAN.md, "Busy stations") is its
// one-shot case: a bell on a busy station that rings once at its QRV, so
// Ragtime can offer to call it, and then comes off.
//
// It is fed what the station list is fed, and hands back the rings; the
// window turns them into desktop notifications. It never transmits. No wx
// and no clock: the time is passed in.
#ifndef RAGTIME_BROADCAST_WATCH_LIST_H
#define RAGTIME_BROADCAST_WATCH_LIST_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

struct Ring
{
    enum class Why
    {
        Heard,      // heard on D2G after a quiet spell (or for the first time this run)
        Qrv,        // it sent QRV: free for a session
    };
    std::string call;
    Why why = Why::Heard;
    bool free = false;              // a Tell me when free bell: offer to call
    std::optional<double> snrDb;    // the burst's, when the host gave one
};

class WatchList
{
public:
    // A watched station heard again after this long without a burst rings
    // again; heard more often, it rings once.
    static constexpr uint64_t QUIET_MS = 30 * 60 * 1000;

    // Our own call is never watched; our session partner never rings.
    void setMyCallsign(const std::string& callsign);
    void setPartner(const std::string& call);

    // The lasting bells, as the operator set them (normalCallsign'd;
    // anything that isn't a callsign is dropped).
    void setCalls(const std::vector<std::string>& calls);
    std::vector<std::string> calls() const;

    // A bell on the station; false if it isn't a callsign or is ours.
    bool watch(const std::string& call);
    // Takes off the lasting bell (a Tell me when free stays).
    void unwatch(const std::string& call);
    bool watching(const std::string& call) const;

    // Tell me when free: rings once at the station's next QRV.
    bool tellWhenFree(const std::string& call);
    void cancelWhenFree(const std::string& call);
    bool waitingFree(const std::string& call) const;

    // Either bell is on the station: the row shows one.
    bool belled(const std::string& call) const { return watching(call) || waitingFree(call); }
    // How the bell came to be, for a tooltip.
    std::string bellTip(const std::string& call) const;

    // The host heard a burst from the station on D2G (or our partner's,
    // which never rings).
    void heard(const std::string& call, uint64_t nowMs, std::optional<double> snr = std::nullopt);
    // A record heard on D2G.
    void onRecord(const protocol::Record& record, uint64_t nowMs);

    // The rings since the last call, oldest first.
    std::vector<Ring> takeRings();

    // Goes up with every change to the bells.
    uint64_t version() const { return version_; }

private:
    struct Watch
    {
        bool bell = false;          // the lasting bell
        bool free = false;          // Tell me when free
        bool heardThisRun = false;
        uint64_t lastHeardMs = 0;
    };

    void burst(const std::string& call, uint64_t nowMs, std::optional<double> snr, bool qrv);

    std::string me_;
    std::string partner_;
    std::map<std::string, Watch> watches_;
    std::vector<Ring> rings_;
    uint64_t version_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
