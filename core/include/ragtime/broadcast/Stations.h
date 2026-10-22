// The station list: every station heard on the D2G group, what it last
// said about itself, and how it shows in the list's columns
// (docs/PLAN.md, "Sometimes we ragchew" and "Main window").
//
// It is fed the records heard on the group and the host's "BCAST HEARD"
// calls; it never transmits. Status always pairs a word with a filled or
// hollow dot, so it never relies on colour alone. No wx and no clock here:
// the time is passed in.
#ifndef RAGTIME_BROADCAST_STATIONS_H
#define RAGTIME_BROADCAST_STATIONS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

// The station list's columns, in the order the table shows them.
enum class Column
{
    Call,
    Status,
    Heard,
    Locator,
    Distance,
    Bearing,
    Key,        // the seal badge; signing comes in Stage 2
    Services,
    Snr,        // needs a host report (an ask for Andrew): listed, never on
};

constexpr int COLUMN_COUNT = 9;
constexpr std::array<Column, COLUMN_COUNT> ALL_COLUMNS = {
    Column::Call, Column::Status, Column::Heard, Column::Locator, Column::Distance,
    Column::Bearing, Column::Key, Column::Services, Column::Snr,
};

// "Call", "Status", "Heard", ..., "SNR".
const char* columnLabel(Column column);
// "call", "status", ...: the name a saved column list uses.
const char* columnKey(Column column);
bool columnFromKey(const std::string& key, Column& out);
// False for SNR, which data2g-host doesn't report for broadcast bursts yet.
bool columnAvailable(Column column);
// The columns shown before the operator picks: all that can be.
std::vector<Column> defaultColumns();

// How long ago, for the Heard column: "just now", "45 s ago", "12 min ago",
// "2 h 5 min ago", "3 days ago".
std::string heardAgo(uint64_t thenMs, uint64_t nowMs);

// A QRL older than this, with nothing since, shows as "QRL? 25 min ago".
constexpr uint64_t QRL_STALE_MS = 20 * 60 * 1000;

struct Station
{
    std::string call;
    uint64_t firstHeardMs = 0;
    uint64_t lastHeardMs = 0;
    size_t texts = 0;               // messages heard from it

    // From its Hello, when one has been heard.
    bool helloHeard = false;
    bool acceptsSessions = false;
    bool automatic = false;
    uint8_t takes = 0;
    std::string locator;            // tidied, or empty
    std::string software;

    // From its latest Status, when one has been heard.
    bool statusHeard = false;
    protocol::Status::State state = protocol::Status::State::Qrv;
    std::string partner;
    uint64_t statusMs = 0;
    // Traffic after a QRT: it is back.
    uint64_t lastTrafficMs = 0;
};

// What the Status column shows.
struct StatusView
{
    std::string word;       // "QRV", "QRL with KC2G", "QRL? 25 min ago", "No sessions", "QRT"
    bool filled = true;     // ● or ○
    std::string how;        // the tooltip: how we know, and when
};

class Stations
{
public:
    // Our own callsign: our own records are never listed.
    void setMyCallsign(const std::string& callsign);
    // Our own locator, for Distance and Bearing.
    void setHome(const std::string& locator);
    const std::string& home() const { return home_; }

    // A record heard on the group.
    void onRecord(const protocol::Record& record, uint64_t nowMs);
    // The host heard a burst from this call ("BCAST <port> HEARD <call>").
    void heard(const std::string& call, uint64_t nowMs);

    void clear();

    const std::vector<Station>& stations() const { return stations_; }
    const Station* find(const std::string& call) const;

    // Goes up with every change, so a view knows when to redraw.
    uint64_t version() const { return version_; }

    // The list sorted by a column. Heard sorts newest first when
    // ascending; stations with no value for the column go last either way.
    std::vector<const Station*> sorted(Column column, bool ascending, uint64_t nowMs) const;

    StatusView status(const Station& station, uint64_t nowMs) const;
    std::optional<double> distanceKm(const Station& station) const;
    std::optional<double> bearingDegrees(const Station& station) const;

    // A cell's text: "W1AW", "● QRV", "3 min ago", "FN31", "3,951 km",
    // "81°", "" for Key until Stage 2, "automatic" for Services.
    std::string cell(const Station& station, Column column, uint64_t nowMs) const;

    // Under the last-heard time in the narrow list: "FN31 · 3,951 km", or
    // "no locator" until one arrives.
    std::string whereLine(const Station& station) const;

private:
    Station& touch(const std::string& call, uint64_t nowMs);

    std::string me_;
    std::string home_;
    std::vector<Station> stations_;
    uint64_t version_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
