// Recent work: who is likely around now, from the path log. The hour
// either side of now, the band the dial is on, the days worked, the three
// orders, and the words under each call.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/radio/PathLog.h"
#include "ragtime/radio/RecentWork.h"

using namespace ragtime;
using namespace ragtime::radio;

namespace
{

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC
const uint64_t HOUR = 60ull * 60 * 1000;
const uint64_t MIN = 60ull * 1000;
const uint64_t DAY = 24 * HOUR;

PathEntry heard(uint64_t at, const std::string& call, const std::string& band, bool session = false)
{
    PathEntry e;
    e.atMs = at;
    e.call = call;
    e.snrDb = -6.0;
    e.band = band;
    e.session = session;
    return e;
}

PathEntry worked(uint64_t at, const std::string& call)
{
    PathEntry e;
    e.atMs = at;
    e.call = call;
    e.worked = true;
    return e;
}

const WorkRow* row(const RecentWork& work, const std::string& call)
{
    for (const WorkRow& r : work.rows)
    {
        if (r.call == call) return &r;
    }
    return nullptr;
}

// 16:30 UTC on day 14: "this hour" is 1500-1800Z.
const uint64_t NOW = T0 + 14 * DAY + 16 * HOUR + 30 * MIN;

std::vector<PathEntry> net()
{
    std::vector<PathEntry> all;
    // KC2G, a friend: heard at this hour on 17 m on days 10 to 13 and just
    // now, and worked on three days, at other hours.
    for (uint64_t d = 10; d < 14; d++) all.push_back(heard(T0 + d * DAY + 16 * HOUR + 5 * MIN, "KC2G", "17 m"));
    all.push_back(heard(NOW - 5 * MIN, "KC2G", "17 m"));
    for (uint64_t d : {2, 5, 8}) all.push_back(worked(T0 + d * DAY + 2 * HOUR, "KC2G"));
    // W1AW heard at this hour on 20 m every day, never worked.
    for (uint64_t d = 0; d <= 14; d++) all.push_back(heard(T0 + d * DAY + 16 * HOUR + 10 * MIN, "W1AW", "20 m"));
    // K7ABC worked as often as KC2G, but heard only at 0300Z.
    for (uint64_t d : {3, 6, 9})
    {
        all.push_back(worked(T0 + d * DAY + 3 * HOUR, "K7ABC"));
        all.push_back(heard(T0 + d * DAY + 3 * HOUR + MIN, "K7ABC", "40 m"));
    }
    // N0OLD only before the span.
    all.push_back(heard(T0 - 3 * DAY, "N0OLD", "20 m"));
    return all;
}

void likelyNow()
{
    RecentWork work = recentWork(net(), NOW, 14, "", WorkOrder::LikelyNow);
    CHECK_EQ(work.hour, 16);
    CHECK_EQ(work.rows.size(), (size_t)3);
    CHECK(row(work, "N0OLD") == nullptr);
    if (work.rows.size() == 3)
    {
        CHECK_EQ(work.rows[0].call, std::string("KC2G"));
        CHECK_EQ(work.rows[1].call, std::string("W1AW"));
        CHECK_EQ(work.rows[2].call, std::string("K7ABC"));
    }
    const WorkRow* kc2g = row(work, "KC2G");
    CHECK(kc2g != nullptr);
    if (kc2g != nullptr)
    {
        CHECK_EQ(kc2g->hourDays, (size_t)5);
        CHECK_EQ(kc2g->workedDays, (size_t)3);
        CHECK_EQ(kc2g->heardDays, (size_t)5);
        CHECK_EQ(kc2g->score, (size_t)24);
        CHECK_EQ(kc2g->hourBand, std::string("17 m"));
        CHECK(kc2g->onNow);
        CHECK_EQ(kc2g->lastWorkedMs, T0 + 8 * DAY + 2 * HOUR);
        CHECK_EQ(workLine(*kc2g), std::string("At this hour 5 days, 17 m \xc2\xb7 worked 3"));
        std::string tip = workTip(*kc2g, work, NOW);
        CHECK(tip.find("KC2G, on now") == 0);
        CHECK(tip.find("1500\xe2\x80\x93" "1800Z") != std::string::npos);
        CHECK(tip.find("= 24.") != std::string::npos);
    }
    const WorkRow* w1aw = row(work, "W1AW");
    CHECK(w1aw != nullptr);
    if (w1aw != nullptr)
    {
        // Day 0's 16:10 is before the span, which starts at 16:30.
        CHECK_EQ(w1aw->hourDays, (size_t)14);
        CHECK_EQ(w1aw->score, (size_t)15);
        CHECK(!w1aw->onNow);
        CHECK_EQ(workLine(*w1aw), std::string("At this hour 14 days, 20 m"));
    }
    const WorkRow* k7abc = row(work, "K7ABC");
    CHECK(k7abc != nullptr);
    if (k7abc != nullptr)
    {
        CHECK_EQ(k7abc->hourDays, (size_t)0);
        CHECK_EQ(k7abc->score, (size_t)4);
        CHECK(k7abc->hourBand.empty());
        CHECK_EQ(workLine(*k7abc), std::string("Worked 3 days"));
        CHECK(workTip(*k7abc, work, NOW).find("never.") != std::string::npos);
    }
}

void bandNow()
{
    // On 20 m, KC2G's 17 m evenings don't count; W1AW's do.
    RecentWork work = recentWork(net(), NOW, 14, "20 m", WorkOrder::LikelyNow);
    CHECK(!work.rows.empty());
    if (!work.rows.empty()) CHECK_EQ(work.rows[0].call, std::string("W1AW"));
    const WorkRow* kc2g = row(work, "KC2G");
    CHECK(kc2g != nullptr && kc2g->hourDays == 0 && kc2g->score == 4);

    // Lines from before the dial was read count on any band.
    std::vector<PathEntry> all = {heard(NOW - DAY, "KC2G", ""), heard(NOW - 2 * DAY, "KC2G", "17 m")};
    work = recentWork(all, NOW, 14, "20 m", WorkOrder::LikelyNow);
    kc2g = row(work, "KC2G");
    CHECK(kc2g != nullptr && kc2g->hourDays == 1 && kc2g->hourBand.empty() && kc2g->heardDays == 2);
}

void orders()
{
    std::vector<PathEntry> all = net();
    // K7ABC worked once more, today at 0300Z.
    all.push_back(worked(T0 + 14 * DAY + 3 * HOUR, "K7ABC"));
    RecentWork work = recentWork(all, NOW, 14, "", WorkOrder::MostWorked);
    CHECK_EQ(work.rows.size(), (size_t)3);
    if (work.rows.size() == 3)
    {
        CHECK_EQ(work.rows[0].call, std::string("K7ABC"));
        CHECK_EQ(work.rows[1].call, std::string("KC2G"));
        CHECK_EQ(work.rows[2].call, std::string("W1AW"));
    }
    work = recentWork(all, NOW, 14, "", WorkOrder::Latest);
    if (work.rows.size() == 3)
    {
        CHECK_EQ(work.rows[0].call, std::string("KC2G"));
        CHECK_EQ(work.rows[1].call, std::string("W1AW"));
        CHECK_EQ(work.rows[2].call, std::string("K7ABC"));
    }
    CHECK_EQ(std::string(workOrderLabel(WorkOrder::LikelyNow)), std::string("Likely now"));
    CHECK_EQ(std::string(workOrderLabel(WorkOrder::MostWorked)), std::string("Most worked"));
    CHECK_EQ(std::string(workOrderLabel(WorkOrder::Latest)), std::string("Latest"));
}

void sessionsAndMidnight()
{
    // A session's bursts are work and bursts heard.
    std::vector<PathEntry> all = {heard(NOW - 2 * HOUR, "W1AW", "20 m", true)};
    RecentWork work = recentWork(all, NOW, 14, "", WorkOrder::LikelyNow);
    const WorkRow* w1aw = row(work, "W1AW");
    CHECK(w1aw != nullptr && w1aw->workedDays == 1 && w1aw->heardDays == 1 && w1aw->hourDays == 0);

    // Only worked: never heard.
    all = {worked(NOW - HOUR, "N0CALL")};
    work = recentWork(all, NOW, 14, "", WorkOrder::LikelyNow);
    const WorkRow* n0call = row(work, "N0CALL");
    CHECK(n0call != nullptr && n0call->heardDays == 0 && n0call->lastHeardMs == 0 && !n0call->onNow);
    if (n0call != nullptr)
    {
        CHECK_EQ(workLine(*n0call), std::string("Worked 1 day"));
        CHECK(workTip(*n0call, work, NOW).find("Not heard in the last 14 days.") != std::string::npos);
    }

    // "This hour" wraps midnight: at 00:30, 23:40 the day before counts.
    uint64_t now = T0 + 5 * DAY + 30 * MIN;
    all = {heard(now - 50 * MIN, "KC2G", "17 m"), heard(now - 26 * HOUR, "KC2G", "17 m")};
    work = recentWork(all, now, 14, "", WorkOrder::LikelyNow);
    const WorkRow* kc2g = row(work, "KC2G");
    CHECK(kc2g != nullptr && kc2g->hourDays == 1 && kc2g->heardDays == 2);
    if (kc2g != nullptr) CHECK_EQ(workLine(*kc2g), std::string("At this hour 1 day, 17 m"));
    CHECK_EQ(hourSpan(0), std::string("2300\xe2\x80\x93" "0200Z"));
    CHECK_EQ(hourSpan(23), std::string("2200\xe2\x80\x93" "0100Z"));

    CHECK(recentWork({}, NOW, 14, "", WorkOrder::LikelyNow).rows.empty());
}

} // namespace

int main()
{
    likelyNow();
    bandNow();
    orders();
    sessionsAndMidnight();
    return ragtime_test::finish();
}
