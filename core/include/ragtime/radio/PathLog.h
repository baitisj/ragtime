// The path log: one line for every burst heard from another station,
// appended and never rewritten, so weeks of who-hears-whom build up for
// the net's path table in Stage 2 (docs/PLAN.md, "Welcome to the D2G
// net"). Each line has the UTC time, the station, the S/N data2g-host
// measured, the burst's mode when the host named it, the band and dial
// from rigctld when Ragtime reads it, and the gear we were sending in.
//
// From the recent lines it draws each station's 24-hour strip by band
// for the station details: "17 m: 2000-0000Z on 5 days, median -6 dB".
//
// No wx, and the time is passed in.
#ifndef RAGTIME_RADIO_PATH_LOG_H
#define RAGTIME_RADIO_PATH_LOG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ragtime
{
namespace radio
{

struct PathEntry
{
    uint64_t atMs = 0;              // UTC; the file keeps whole seconds
    std::string call;
    std::optional<double> snrDb;    // none from a host older than Data2G PR #98
    std::string mode;               // Data2G's name for the burst's mode; empty when the host didn't say
    std::string band;               // "17 m"; empty while the dial isn't read
    uint64_t dialHz = 0;            // 0 while the dial isn't read
    std::string gear;               // the mode we were sending in, Data2G's name; empty for the host's
    bool session = false;           // our session partner's burst, else a D2G broadcast
};

// "2026-10-25T16:30:12Z", and back (false for anything else).
std::string utcStamp(uint64_t ms);
bool parseUtcStamp(const std::string& text, uint64_t& ms);

// One line of the file, without its newline: tab-separated time, call,
// S/N, mode, band, dial, gear, and "D2G" or "session"; an unknown field is
// empty. And back: false for a line that isn't one.
std::string pathLine(const PathEntry& entry);
bool parsePathLine(const std::string& line, PathEntry& entry);

// One hour of the day (UTC) on one band, over the strip's days.
struct HourCell
{
    size_t bursts = 0;
    size_t days = 0;                // distinct days with a burst in this hour
    std::optional<double> medianDb;
};

// One band's row of the strip.
struct BandRow
{
    std::string band;               // empty: the dial wasn't read
    std::array<HourCell, 24> hours;
    size_t bursts = 0;
    size_t days = 0;
    std::optional<double> medianDb;
};

// A station's strip: a row for each band it was heard on in the last
// `days` days, lowest band first and the dial-unknown row last.
struct PathStrip
{
    std::string call;
    size_t days = 0;                // the span it covers
    std::vector<BandRow> bands;
    uint64_t lastMs = 0;            // the newest burst in it
};

PathStrip pathStrip(const std::vector<const PathEntry*>& entries, const std::string& call, uint64_t nowMs,
                    size_t days);

// A row in words: "17 m: 2000-0000Z on 5 days, median -6 dB", or
// "Band not known: 0400-0600Z on 1 day" for heard while the dial wasn't
// read. Hours with a burst are joined into runs; three at most are named.
std::string rowSummary(const BandRow& row);

class PathLog
{
public:
    // Kept in memory for the strips: this many days back.
    static constexpr size_t KEEP_DAYS = 14;
    static constexpr uint64_t DAY_MS = 24ull * 60 * 60 * 1000;

    ~PathLog();

    // The file to append to, created when the first line goes. Its last
    // KEEP_DAYS days are read back, from the end, so a long log opens
    // quickly. An empty path keeps the log in memory only.
    void setPath(const std::string& path, uint64_t nowMs);
    const std::string& path() const { return path_; }

    // Appends a line to the file at once, and keeps it for the strips.
    void add(const PathEntry& entry);

    // The entries kept, oldest first; and a station's, since a time.
    const std::vector<PathEntry>& entries() const { return entries_; }
    std::vector<const PathEntry*> of(const std::string& call, uint64_t sinceMs) const;

    // Drops what is older than KEEP_DAYS from memory (never from the file).
    void trim(uint64_t nowMs);

    // Goes up with every line.
    uint64_t version() const { return version_; }
    // Why the file couldn't be written, or "".
    const std::string& problem() const { return problem_; }

private:
    std::string path_;
    std::ofstream out_;
    std::vector<PathEntry> entries_;
    uint64_t version_ = 0;
    std::string problem_;
};

// The last lines of a file of entries written since sinceMs, read from the
// end in growing chunks. Lines it can't read are skipped.
std::vector<PathEntry> readPathTail(const std::string& path, uint64_t sinceMs);

} // namespace radio
} // namespace ragtime

#endif
