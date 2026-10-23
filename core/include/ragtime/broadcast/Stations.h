// The station list: every station heard on the D2G group, what it last
// said about itself, and how it shows in the list's columns
// (docs/PLAN.md, "Sometimes we ragchew" and "Main window").
//
// It is fed the records heard on the group and the host's "BCAST HEARD"
// calls and SNRs (and the session partner's, from SN); it never transmits. Status always pairs a word with a filled or
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

#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

// The station list's columns. Their numbers are saved (the sort column)
// and are menu ids, so they never change; ALL_COLUMNS is the order the
// table shows them in.
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
    Snr,        // the last burst's, from data2g-host since Data2G PR #98
};

constexpr int COLUMN_COUNT = 9;
constexpr std::array<Column, COLUMN_COUNT> ALL_COLUMNS = {
    Column::Call, Column::Status, Column::Heard, Column::Snr, Column::Locator,
    Column::Distance, Column::Bearing, Column::Key, Column::Services,
};

// "Call", "Status", "Heard", ..., "SNR".
const char* columnLabel(Column column);
// "call", "status", ...: the name a saved column list uses.
const char* columnKey(Column column);
bool columnFromKey(const std::string& key, Column& out);
// The columns shown before the operator picks: all of them.
std::vector<Column> defaultColumns();

// How long ago, for the Heard column: "just now", "45 s ago", "12 min ago",
// "2 h 5 min ago", "3 days ago".
std::string heardAgo(uint64_t thenMs, uint64_t nowMs);

// A QRL older than this, with nothing since, shows as "QRL? 25 min ago".
constexpr uint64_t QRL_STALE_MS = 20 * 60 * 1000;
// The gearshift's Auto serves the weakest station heard this recently.
constexpr uint64_t PATH_WINDOW_MS = 15 * 60 * 1000;

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

    // Its bursts' SNR as data2g-host heard them (dB, average power in
    // 2500 Hz): the last one, and the last few for the gearshift. None from
    // a host older than Data2G PR #98.
    std::optional<double> snrDb;
    uint64_t snrMs = 0;
    gearbox::PathSnr path;
};

// The path the gearshift's Auto shifts for: the weakest station heard
// lately, by the mean of its last few bursts.
struct PathReport
{
    std::string call;
    double dB = 0.0;
    size_t bursts = 0;
};

// What the Status column shows.
struct StatusView
{
    std::string word;       // "QRV", "QRL with KC2G", "QRL? 25 min ago", "No sessions", "QRT",
                            // "In session with you", "Calling"
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
    // The host heard a burst from this call ("BCAST <port> HEARD <call>
    // [snr]", or SN for the session partner), with its SNR when it gave one.
    void heard(const std::string& call, uint64_t nowMs, std::optional<double> snr = std::nullopt);

    // The weakest of the stations whose SNR came in the last
    // PATH_WINDOW_MS; none when no station has one.
    std::optional<PathReport> weakestPath(uint64_t nowMs) const;

    void clear();

    // The station a session is up with, or being called: listed at once
    // (a caller may never have been heard on the group), pinned at the top
    // of every sort, and shown "In session with you" or "Calling". Empty
    // for none.
    void setPartner(const std::string& call, bool connected, uint64_t nowMs);
    const std::string& partner() const { return partner_; }

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
    // "81°", "" for Key until Stage 2, "automatic" for Services, "+3.5 dB" for SNR.
    std::string cell(const Station& station, Column column, uint64_t nowMs) const;

    // Under the last-heard time in the narrow list: "FN31 · 3,951 km", or
    // "no locator" until one arrives.
    std::string whereLine(const Station& station) const;

private:
    Station& touch(const std::string& call, uint64_t nowMs);

    std::string me_;
    std::string home_;
    std::string partner_;
    bool partnerConnected_ = false;
    std::vector<Station> stations_;
    uint64_t version_ = 0;
};

} // namespace broadcast
} // namespace ragtime

#endif
