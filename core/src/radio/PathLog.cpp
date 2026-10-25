#include "ragtime/radio/PathLog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/radio/Bands.h"

#include "../LogLines.h"

namespace ragtime
{
namespace radio
{

namespace
{

using loglines::escape;
using loglines::fields;
using loglines::number;
using loglines::unescape;

constexpr uint64_t HOUR_MS = 60ull * 60 * 1000;

// Days since 1970-01-01 for a civil date, and back (H. Hinnant's
// algorithms), so no time zone or timegm() comes into it.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool digits(const std::string& text, size_t at, size_t count, unsigned& value)
{
    value = 0;
    for (size_t i = at; i < at + count; i++)
    {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (unsigned)(text[i] - '0');
    }
    return true;
}

std::optional<double> median(std::vector<double> values)
{
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

std::string hourText(int hour)
{
    char text[8];
    std::snprintf(text, sizeof text, "%02d00", hour % 24);
    return text;
}

} // namespace

std::string utcStamp(uint64_t ms)
{
    uint64_t s = ms / 1000;
    int64_t y = 0;
    unsigned m = 0, d = 0;
    civilFromDays((int64_t)(s / 86400), y, m, d);
    unsigned rest = (unsigned)(s % 86400);
    char text[40];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02uT%02u:%02u:%02uZ", (long long)y, m, d, rest / 3600,
                  rest / 60 % 60, rest % 60);
    return text;
}

bool parseUtcStamp(const std::string& text, uint64_t& ms)
{
    unsigned y, mo, d, h, mi, s;
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':'
        || text[16] != ':' || text[19] != 'Z')
        return false;
    if (!digits(text, 0, 4, y) || !digits(text, 5, 2, mo) || !digits(text, 8, 2, d) || !digits(text, 11, 2, h)
        || !digits(text, 14, 2, mi) || !digits(text, 17, 2, s))
        return false;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    int64_t days = daysFromCivil(y, mo, d);
    ms = ((uint64_t)days * 86400 + h * 3600 + mi * 60 + s) * 1000;
    return true;
}

std::string pathLine(const PathEntry& e)
{
    std::string snr;
    if (e.snrDb)
    {
        char text[16];
        std::snprintf(text, sizeof text, "%.1f", *e.snrDb);
        snr = text;
    }
    return loglines::join({utcStamp(e.atMs), escape(e.call), snr, escape(e.mode), escape(e.band),
                           e.dialHz == 0 ? std::string() : std::to_string(e.dialHz), escape(e.gear),
                           e.session ? "session" : "D2G"});
}

bool parsePathLine(const std::string& line, PathEntry& e)
{
    std::vector<std::string> f = fields(line);
    if (f.size() < 8) return false;
    PathEntry out;
    if (!parseUtcStamp(f[0], out.atMs)) return false;
    out.call = unescape(f[1]);
    if (out.call.empty()) return false;
    if (!f[2].empty())
    {
        char* end = nullptr;
        double dB = std::strtod(f[2].c_str(), &end);
        if (end == f[2].c_str() || *end != '\0') return false;
        out.snrDb = dB;
    }
    out.mode = unescape(f[3]);
    out.band = unescape(f[4]);
    if (!f[5].empty() && !number(f[5], out.dialHz)) return false;
    out.gear = unescape(f[6]);
    if (f[7] != "session" && f[7] != "D2G") return false;
    out.session = f[7] == "session";
    e = std::move(out);
    return true;
}

//-------------------------------------------------------------------------
// The strip
//-------------------------------------------------------------------------

PathStrip pathStrip(const std::vector<const PathEntry*>& entries, const std::string& call, uint64_t nowMs,
                    size_t days)
{
    PathStrip strip;
    strip.call = call;
    strip.days = days;
    uint64_t since = nowMs > days * PathLog::DAY_MS ? nowMs - days * PathLog::DAY_MS : 0;

    struct Gather
    {
        std::array<std::vector<double>, 24> snrs;
        std::array<std::set<uint64_t>, 24> days;
        std::array<size_t, 24> bursts{};
        std::vector<double> all;
        std::set<uint64_t> allDays;
    };
    std::map<std::string, Gather> byBand;
    for (const PathEntry* e : entries)
    {
        if (e == nullptr || e->call != call || e->atMs < since || e->atMs > nowMs + HOUR_MS) continue;
        Gather& g = byBand[e->band];
        size_t hour = (size_t)(e->atMs / HOUR_MS % 24);
        uint64_t day = e->atMs / PathLog::DAY_MS;
        g.bursts[hour]++;
        g.days[hour].insert(day);
        g.allDays.insert(day);
        if (e->snrDb)
        {
            g.snrs[hour].push_back(*e->snrDb);
            g.all.push_back(*e->snrDb);
        }
        strip.lastMs = std::max(strip.lastMs, e->atMs);
    }
    for (auto& [band, g] : byBand)
    {
        BandRow row;
        row.band = band;
        for (size_t h = 0; h < 24; h++)
        {
            row.hours[h].bursts = g.bursts[h];
            row.hours[h].days = g.days[h].size();
            row.hours[h].medianDb = median(g.snrs[h]);
            row.bursts += g.bursts[h];
        }
        row.days = g.allDays.size();
        row.medianDb = median(g.all);
        strip.bands.push_back(std::move(row));
    }
    std::sort(strip.bands.begin(), strip.bands.end(), [](const BandRow& a, const BandRow& b) {
        size_t oa = a.band.empty() ? SIZE_MAX : bandOrder(a.band);
        size_t ob = b.band.empty() ? SIZE_MAX : bandOrder(b.band);
        return oa != ob ? oa < ob : a.band < b.band;
    });
    return strip;
}

std::string rowSummary(const BandRow& row)
{
    std::string out = (row.band.empty() ? std::string("Band not known") : row.band) + ": ";

    // Runs of hours with bursts, around the clock.
    struct Run
    {
        int start = 0;
        int length = 0;
        size_t bursts = 0;
    };
    std::vector<Run> runs;
    int lit = 0;
    for (const HourCell& c : row.hours) lit += c.bursts > 0 ? 1 : 0;
    if (lit == 24)
    {
        out += "all day";
    }
    else if (lit > 0)
    {
        // Start just after a quiet hour, so a run over midnight stays whole.
        int quiet = 0;
        while (row.hours[(size_t)quiet].bursts > 0) quiet++;
        for (int i = 1; i <= 24; i++)
        {
            int h = (quiet + i) % 24;
            if (row.hours[(size_t)h].bursts == 0) continue;
            int before = (h + 23) % 24;
            if (runs.empty() || row.hours[(size_t)before].bursts == 0) runs.push_back(Run{h, 0, 0});
            runs.back().length++;
            runs.back().bursts += row.hours[(size_t)h].bursts;
        }
        bool more = runs.size() > 3;
        if (more)
        {
            // The three busiest, named in clock order.
            std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.bursts > b.bursts; });
            runs.resize(3);
        }
        std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.start < b.start; });
        for (size_t i = 0; i < runs.size(); i++)
        {
            if (i > 0) out += i + 1 == runs.size() && !more ? " and " : ", ";
            out += hourText(runs[i].start) + "\xE2\x80\x93" + hourText(runs[i].start + runs[i].length) + "Z";
        }
        if (more) out += " and other hours";
    }
    out += " on " + std::to_string(row.days) + (row.days == 1 ? " day" : " days");
    if (row.medianDb) out += ", median " + gearbox::formatDb(*row.medianDb);
    return out;
}

//-------------------------------------------------------------------------
// The file
//-------------------------------------------------------------------------

std::vector<PathEntry> readPathTail(const std::string& path, uint64_t sinceMs)
{
    std::vector<PathEntry> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size <= 0) return out;

    // Back from the end until the chunk starts before sinceMs (the file is
    // in time order) or holds the whole file.
    std::streamoff chunk = 256 * 1024;
    std::string text;
    while (true)
    {
        std::streamoff start = std::max<std::streamoff>(0, size - chunk);
        in.clear();
        in.seekg(start);
        text.assign((size_t)(size - start), '\0');
        in.read(&text[0], (std::streamsize)text.size());
        text.resize((size_t)in.gcount());
        size_t first = 0;
        if (start > 0)
        {
            // The chunk's first line is likely cut short.
            first = text.find('\n');
            first = first == std::string::npos ? text.size() : first + 1;
        }
        size_t eol = text.find('\n', first);
        PathEntry e;
        bool older = eol != std::string::npos && parsePathLine(text.substr(first, eol - first), e) && e.atMs < sinceMs;
        if (start == 0 || older)
        {
            text.erase(0, first);
            break;
        }
        chunk *= 4;
    }

    size_t at = 0;
    while (at < text.size())
    {
        size_t eol = text.find('\n', at);
        std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? text.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        PathEntry e;
        if (parsePathLine(line, e) && e.atMs >= sinceMs) out.push_back(std::move(e));
    }
    return out;
}

PathLog::~PathLog() = default;

void PathLog::setPath(const std::string& path, uint64_t nowMs)
{
    if (out_.is_open()) out_.close();
    path_ = path;
    problem_.clear();
    entries_.clear();
    if (!path_.empty())
    {
        uint64_t since = nowMs > KEEP_DAYS * DAY_MS ? nowMs - KEEP_DAYS * DAY_MS : 0;
        entries_ = readPathTail(path_, since);
    }
    version_++;
}

void PathLog::add(const PathEntry& entry)
{
    PathEntry e = entry;
    e.atMs = e.atMs / 1000 * 1000;
    entries_.push_back(e);
    version_++;
    if (path_.empty()) return;
    if (!out_.is_open())
    {
        out_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
        if (!out_)
        {
            problem_ = "Can't write the path log " + path_;
            out_.clear();
            return;
        }
    }
    out_ << pathLine(e) << '\n';
    out_.flush();
    if (!out_)
    {
        problem_ = "Can't write the path log " + path_;
        out_.close();
        out_.clear();
    }
    else
    {
        problem_.clear();
    }
}

std::vector<const PathEntry*> PathLog::of(const std::string& call, uint64_t sinceMs) const
{
    std::vector<const PathEntry*> out;
    for (const PathEntry& e : entries_)
    {
        if (e.call == call && e.atMs >= sinceMs) out.push_back(&e);
    }
    return out;
}

void PathLog::trim(uint64_t nowMs)
{
    uint64_t since = nowMs > KEEP_DAYS * DAY_MS ? nowMs - KEEP_DAYS * DAY_MS : 0;
    auto keep = std::find_if(entries_.begin(), entries_.end(), [since](const PathEntry& e) { return e.atMs >= since; });
    if (keep == entries_.begin()) return;
    entries_.erase(entries_.begin(), keep);
    version_++;
}

} // namespace radio
} // namespace ragtime
