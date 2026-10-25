// Recent work: the stations heard and worked in the path log's last days,
// likely ones first. Every burst heard teaches Ragtime when and on which
// band a station gets through to us, so it can say who is likely around
// now: the friends to look for at this hour, the best time to check in.
//
// Two counts make a station's place, over the days the path log keeps:
//
//   at this hour   the days it was heard within an hour either side of the
//                  hour now (UTC), on the band the dial is on when Ragtime
//                  reads it (lines from before the dial was read count on
//                  any band)
//   worked         the days we worked it: a session, or a message between
//                  it and us (PathEntry::worked)
//
// Likely now ranks by (1 + days worked) x (1 + days at this hour): a
// friend worked on 3 days and heard at this hour on 5 scores 4 x 6 = 24,
// ahead of a stranger heard at this hour every day of 14 (1 x 15 = 15),
// and ahead of the same friend at an hour it is never heard (4 x 1 = 4).
//
// No wx, and the time is passed in.
#ifndef RAGTIME_RADIO_RECENT_WORK_H
#define RAGTIME_RADIO_RECENT_WORK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/radio/PathLog.h"

namespace ragtime
{
namespace radio
{

// How Recent work is sorted. The numbers are saved, so they never change.
enum class WorkOrder
{
    LikelyNow,      // (1 + worked) x (1 + at this hour), the default
    MostWorked,     // the most days worked
    Latest,         // heard or worked most lately
};

// "Likely now", "Most worked", "Latest".
const char* workOrderLabel(WorkOrder order);

// "This hour" is the hour now and one either side.
constexpr int HOUR_SPREAD = 1;
// Heard this lately: on now.
constexpr uint64_t ON_NOW_MS = 15 * 60 * 1000;

struct WorkRow
{
    std::string call;
    size_t heardDays = 0;           // days heard at all
    size_t hourDays = 0;            // days heard at this hour (on the band now, when known)
    size_t workedDays = 0;
    std::string hourBand;           // the band it was heard on most at this hour; empty if none known
    uint64_t lastHeardMs = 0;       // 0: never heard, only worked
    uint64_t lastWorkedMs = 0;      // 0: never worked
    bool onNow = false;             // heard in the last ON_NOW_MS
    size_t score = 0;               // (1 + workedDays) x (1 + hourDays)
};

struct RecentWork
{
    size_t days = 0;                // the span looked at
    int hour = 0;                   // the hour now, UTC
    std::string band;               // the band the dial is on, or ""
    std::vector<WorkRow> rows;
};

// The stations in the entries from the last `days` days, sorted. Our own
// call is never in the path log, so never here.
RecentWork recentWork(const std::vector<PathEntry>& entries, uint64_t nowMs, size_t days, const std::string& bandNow,
                      WorkOrder order);

// Under the call: "At this hour 5 days, 17 m · worked 3", "Worked 2 days",
// "Heard 4 days".
std::string workLine(const WorkRow& row);

// The hours "this hour" covers: "1500-1800Z" (with an en dash).
std::string hourSpan(int hour);

// The row's tooltip: what it was heard and worked, and its score.
std::string workTip(const WorkRow& row, const RecentWork& work, uint64_t nowMs);

} // namespace radio
} // namespace ragtime

#endif
