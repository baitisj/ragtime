#include "ragtime/radio/RecentWork.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "ragtime/broadcast/Stations.h"
#include "ragtime/radio/Bands.h"

namespace ragtime
{
namespace radio
{

namespace
{

constexpr uint64_t HOUR_MS = 60ull * 60 * 1000;

std::string days(size_t n)
{
    return std::to_string(n) + (n == 1 ? " day" : " days");
}

// Within HOUR_SPREAD of the hour now, round midnight.
bool atThisHour(uint64_t ms, int hour)
{
    int h = (int)(ms / HOUR_MS % 24);
    int apart = (h - hour + 24) % 24;
    return apart <= HOUR_SPREAD || apart >= 24 - HOUR_SPREAD;
}

} // namespace

const char* workOrderLabel(WorkOrder order)
{
    switch (order)
    {
    case WorkOrder::LikelyNow: return "Likely now";
    case WorkOrder::MostWorked: return "Most worked";
    case WorkOrder::Latest: return "Latest";
    }
    return "";
}

RecentWork recentWork(const std::vector<PathEntry>& entries, uint64_t nowMs, size_t span, const std::string& bandNow,
                      WorkOrder order)
{
    RecentWork work;
    work.days = span;
    work.hour = (int)(nowMs / HOUR_MS % 24);
    work.band = bandNow;
    uint64_t since = nowMs > span * PathLog::DAY_MS ? nowMs - span * PathLog::DAY_MS : 0;

    struct Gather
    {
        std::set<uint64_t> heard, hour, worked;
        std::map<std::string, std::set<uint64_t>> hourBands;
        uint64_t lastHeardMs = 0, lastWorkedMs = 0;
    };
    std::map<std::string, Gather> byCall;
    for (const PathEntry& e : entries)
    {
        if (e.call.empty() || e.atMs < since || e.atMs > nowMs + HOUR_MS) continue;
        Gather& g = byCall[e.call];
        uint64_t day = e.atMs / PathLog::DAY_MS;
        // A session's bursts are work as well as bursts heard.
        if (e.worked || e.session)
        {
            g.worked.insert(day);
            g.lastWorkedMs = std::max(g.lastWorkedMs, e.atMs);
        }
        if (e.worked) continue;
        g.heard.insert(day);
        g.lastHeardMs = std::max(g.lastHeardMs, e.atMs);
        if (!atThisHour(e.atMs, work.hour)) continue;
        if (!bandNow.empty() && !e.band.empty() && e.band != bandNow) continue;
        g.hour.insert(day);
        if (!e.band.empty()) g.hourBands[e.band].insert(day);
    }

    for (auto& [call, g] : byCall)
    {
        WorkRow row;
        row.call = call;
        row.heardDays = g.heard.size();
        row.hourDays = g.hour.size();
        row.workedDays = g.worked.size();
        size_t most = 0;
        for (const auto& [band, d] : g.hourBands)
        {
            // Most days, and the lower band on a tie.
            if (d.size() > most || (d.size() == most && bandOrder(band) < bandOrder(row.hourBand)))
            {
                most = d.size();
                row.hourBand = band;
            }
        }
        row.lastHeardMs = g.lastHeardMs;
        row.lastWorkedMs = g.lastWorkedMs;
        row.onNow = g.lastHeardMs != 0 && g.lastHeardMs + ON_NOW_MS >= nowMs;
        row.score = (1 + row.workedDays) * (1 + row.hourDays);
        work.rows.push_back(std::move(row));
    }

    auto latest = [](const WorkRow& r) { return std::max(r.lastHeardMs, r.lastWorkedMs); };
    std::sort(work.rows.begin(), work.rows.end(), [&](const WorkRow& a, const WorkRow& b) {
        switch (order)
        {
        case WorkOrder::LikelyNow:
            return std::make_tuple(b.score, b.hourDays, b.workedDays, latest(b), a.call)
                   < std::make_tuple(a.score, a.hourDays, a.workedDays, latest(a), b.call);
        case WorkOrder::MostWorked:
            return std::make_tuple(b.workedDays, b.lastWorkedMs, b.heardDays, a.call)
                   < std::make_tuple(a.workedDays, a.lastWorkedMs, a.heardDays, b.call);
        case WorkOrder::Latest:
            break;
        }
        return std::make_tuple(latest(b), a.call) < std::make_tuple(latest(a), b.call);
    });
    return work;
}

std::string workLine(const WorkRow& row)
{
    std::string line;
    if (row.hourDays > 0)
    {
        line = "At this hour " + days(row.hourDays);
        if (!row.hourBand.empty()) line += ", " + row.hourBand;
    }
    if (row.workedDays > 0)
        line += line.empty() ? "Worked " + days(row.workedDays) : " \xc2\xb7 worked " + std::to_string(row.workedDays);
    if (line.empty()) line = "Heard " + days(row.heardDays);
    return line;
}

std::string hourSpan(int hour)
{
    char text[32];
    std::snprintf(text, sizeof text, "%02d00\xe2\x80\x93%02d00Z", (hour - HOUR_SPREAD + 24) % 24,
                  (hour + HOUR_SPREAD + 1) % 24);
    return text;
}

std::string workTip(const WorkRow& row, const RecentWork& work, uint64_t nowMs)
{
    std::string tip = row.call;
    if (row.onNow) tip += ", on now";
    tip += "\n";
    if (row.heardDays == 0)
    {
        tip += "Not heard in the last " + days(work.days) + ".";
    }
    else
    {
        tip += "Heard on " + std::to_string(row.heardDays) + " of the last " + days(work.days) + ", last "
               + broadcast::heardAgo(row.lastHeardMs, nowMs) + ".";
        tip += "\nAt this hour (" + hourSpan(work.hour) + (work.band.empty() ? "" : ", " + work.band) + "): ";
        if (row.hourDays == 0)
            tip += "never.";
        else
            tip += days(row.hourDays) + (row.hourBand.empty() || !work.band.empty() ? "" : ", most on " + row.hourBand)
                   + ".";
    }
    if (row.workedDays > 0)
        tip += "\nWorked on " + days(row.workedDays) + ", last " + broadcast::heardAgo(row.lastWorkedMs, nowMs) + ".";
    else
        tip += "\nNot worked: no session or message with it.";
    tip += "\nLikely now: (1 + " + std::to_string(row.workedDays) + " worked) \xc3\x97 (1 + "
           + std::to_string(row.hourDays) + " at this hour) = " + std::to_string(row.score) + ".";
    return tip;
}

} // namespace radio
} // namespace ragtime
