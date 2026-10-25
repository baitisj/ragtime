// The watch list: a bell on a call never heard rings at its first burst,
// once for a run of bursts and again after a quiet spell; a QRV rings,
// taking the place of a heard ring for the same burst; Tell me when free
// rings once and comes off; our partner never rings; our own call can't be
// watched; and the lasting bells round-trip as a list.
#include <cstdint>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/WatchList.h"

using namespace ragtime;
using broadcast::Ring;
using broadcast::WatchList;

namespace
{

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC
const uint64_t MIN = 60 * 1000;

protocol::Record status(const std::string& call, protocol::Status::State state, const std::string& partner = "")
{
    protocol::Record r;
    r.header.callsign = call;
    protocol::Status s;
    s.state = state;
    s.partner = partner;
    r.body = s;
    return r;
}

protocol::Record text(const std::string& call)
{
    protocol::Record r;
    r.header.callsign = call;
    protocol::Text t;
    t.text = "CQ";
    r.body = t;
    return r;
}

void bells()
{
    WatchList w;
    w.setMyCallsign("AG7EW");
    CHECK(!w.watch("AG7EW"));
    CHECK(!w.watch("not a call!"));
    CHECK(w.watch("kc2g"));
    CHECK(w.watching("KC2G"));
    CHECK(w.belled("KC2G"));
    CHECK(!w.bellTip("KC2G").empty());

    // Unwatched stations never ring.
    w.heard("W1AW", T0, -3.0);
    CHECK(w.takeRings().empty());

    // The first burst rings, with its S/N; the records in it add nothing.
    w.heard("KC2G", T0, -7.5);
    w.onRecord(text("KC2G"), T0);
    std::vector<Ring> rings = w.takeRings();
    CHECK_EQ(rings.size(), (size_t)1);
    if (rings.size() == 1)
    {
        CHECK_EQ(rings[0].call, std::string("KC2G"));
        CHECK(rings[0].why == Ring::Why::Heard);
        CHECK(!rings[0].free);
        CHECK(rings[0].snrDb && *rings[0].snrDb == -7.5);
    }
    // More bursts in the next half hour: quiet.
    for (uint64_t t = T0 + MIN; t < T0 + 25 * MIN; t += 5 * MIN) w.heard("KC2G", t, -7.0);
    CHECK(w.takeRings().empty());
    // After a quiet spell of QUIET_MS: rings again.
    w.heard("KC2G", T0 + 25 * MIN + WatchList::QUIET_MS, -5.0);
    CHECK_EQ(w.takeRings().size(), (size_t)1);

    // A QRV always rings, and takes the place of the heard ring from the
    // same burst, keeping its S/N.
    uint64_t later = T0 + 5 * WatchList::QUIET_MS;
    w.heard("KC2G", later, -9.0);
    w.onRecord(status("KC2G", protocol::Status::State::Qrv), later);
    rings = w.takeRings();
    CHECK_EQ(rings.size(), (size_t)1);
    if (rings.size() == 1)
    {
        CHECK(rings[0].why == Ring::Why::Qrv);
        CHECK(rings[0].snrDb && *rings[0].snrDb == -9.0);
    }
    w.onRecord(status("KC2G", protocol::Status::State::Qrl, "W1AW"), later + MIN);
    CHECK(w.takeRings().empty());
    w.onRecord(status("KC2G", protocol::Status::State::Qrv), later + 2 * MIN);
    CHECK_EQ(w.takeRings().size(), (size_t)1);

    // Our partner never rings.
    w.setPartner("KC2G");
    w.onRecord(status("KC2G", protocol::Status::State::Qrv), later + 3 * MIN);
    w.heard("KC2G", later + 3 * MIN + 2 * WatchList::QUIET_MS);
    CHECK(w.takeRings().empty());
    w.setPartner("");

    w.unwatch("KC2G");
    CHECK(!w.belled("KC2G"));
    w.onRecord(status("KC2G", protocol::Status::State::Qrv), later + 4 * MIN);
    CHECK(w.takeRings().empty());
}

void whenFree()
{
    WatchList w;
    w.setMyCallsign("AG7EW");
    CHECK(w.tellWhenFree("W1AW"));
    CHECK(w.waitingFree("W1AW"));
    CHECK(w.belled("W1AW"));
    CHECK(!w.watching("W1AW"));
    uint64_t v = w.version();

    // Heard while busy: no ring, it isn't a lasting bell.
    w.heard("W1AW", T0, -2.0);
    w.onRecord(status("W1AW", protocol::Status::State::Qrl, "KC2G"), T0);
    CHECK(w.takeRings().empty());

    // Its QRV rings once, offering to call, and the bell comes off.
    w.onRecord(status("W1AW", protocol::Status::State::Qrv), T0 + 10 * MIN);
    std::vector<Ring> rings = w.takeRings();
    CHECK_EQ(rings.size(), (size_t)1);
    if (rings.size() == 1)
    {
        CHECK(rings[0].why == Ring::Why::Qrv);
        CHECK(rings[0].free);
    }
    CHECK(!w.belled("W1AW"));
    CHECK(w.version() != v);
    w.onRecord(status("W1AW", protocol::Status::State::Qrv), T0 + 11 * MIN);
    CHECK(w.takeRings().empty());

    // On a watched station it rings once with free, and the lasting bell stays.
    w.watch("KC2G");
    w.heard("KC2G", T0);
    w.takeRings();
    w.tellWhenFree("KC2G");
    w.onRecord(status("KC2G", protocol::Status::State::Qrv), T0 + MIN);
    rings = w.takeRings();
    CHECK_EQ(rings.size(), (size_t)1);
    if (rings.size() == 1) CHECK(rings[0].free);
    CHECK(w.watching("KC2G"));
    CHECK(!w.waitingFree("KC2G"));

    // Cancelled before the QRV: nothing.
    w.tellWhenFree("N0CALL");
    w.cancelWhenFree("N0CALL");
    CHECK(!w.belled("N0CALL"));
}

void lasting()
{
    WatchList w;
    w.setMyCallsign("AG7EW");
    w.setCalls({"kc2g", "W1AW", "AG7EW", "bad call", ""});
    std::vector<std::string> calls = w.calls();
    CHECK_EQ(calls.size(), (size_t)2);
    if (calls.size() == 2)
    {
        CHECK_EQ(calls[0], std::string("KC2G"));
        CHECK_EQ(calls[1], std::string("W1AW"));
    }
    // A Tell me when free isn't a lasting bell, and setCalls leaves it be.
    w.tellWhenFree("N7XYZ");
    w.setCalls({"KC2G"});
    CHECK_EQ(w.calls().size(), (size_t)1);
    CHECK(w.waitingFree("N7XYZ"));
    CHECK(!w.watching("W1AW"));
}

} // namespace

int main()
{
    bells();
    whenFree();
    lasting();
    return ragtime_test::finish();
}
