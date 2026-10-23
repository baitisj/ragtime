#include "ragtime/broadcast/Stations.h"

#include <algorithm>
#include <cmath>

#include "ragtime/globus/Globe.h"

namespace ragtime
{
namespace broadcast
{

namespace
{

struct ColumnName
{
    Column column;
    const char* label;
    const char* key;
};

constexpr ColumnName COLUMN_NAMES[COLUMN_COUNT] = {
    {Column::Call, "Call", "call"},
    {Column::Status, "Status", "status"},
    {Column::Heard, "Heard", "heard"},
    {Column::Locator, "Locator", "locator"},
    {Column::Distance, "Distance", "distance"},
    {Column::Bearing, "Bearing", "bearing"},
    {Column::Key, "Key", "key"},
    {Column::Services, "Services", "services"},
    {Column::Snr, "SNR", "snr"},
};

const char* const DOT_FILLED = "\xe2\x97\x8f";  // ●
const char* const DOT_HOLLOW = "\xe2\x97\x8b";  // ○

// The order the Status column sorts in: available first.
int statusRank(const StatusView& view)
{
    if (view.word == "QRV") return 0;
    if (view.word.compare(0, 4, "QRL ") == 0 || view.word == "QRL") return 1;
    if (view.word.compare(0, 4, "QRL?") == 0) return 2;
    if (view.word == "No sessions") return 3;
    return 4;
}

} // namespace

const char* columnLabel(Column column)
{
    return COLUMN_NAMES[(int)column].label;
}

const char* columnKey(Column column)
{
    return COLUMN_NAMES[(int)column].key;
}

bool columnFromKey(const std::string& key, Column& out)
{
    for (const ColumnName& name : COLUMN_NAMES)
    {
        if (key == name.key)
        {
            out = name.column;
            return true;
        }
    }
    return false;
}

std::vector<Column> defaultColumns()
{
    return std::vector<Column>(ALL_COLUMNS.begin(), ALL_COLUMNS.end());
}

std::string heardAgo(uint64_t thenMs, uint64_t nowMs)
{
    uint64_t s = nowMs > thenMs ? (nowMs - thenMs) / 1000 : 0;
    if (s < 10) return "just now";
    if (s < 60) return std::to_string(s) + " s ago";
    uint64_t min = s / 60;
    if (min < 60) return std::to_string(min) + " min ago";
    uint64_t h = min / 60;
    if (h < 48)
    {
        std::string out = std::to_string(h) + " h";
        if (h < 10 && min % 60 != 0) out += " " + std::to_string(min % 60) + " min";
        return out + " ago";
    }
    return std::to_string(h / 24) + " days ago";
}

//-------------------------------------------------------------------------
// The list
//-------------------------------------------------------------------------

void Stations::setMyCallsign(const std::string& callsign)
{
    me_ = protocol::normalCallsign(callsign);
    stations_.erase(std::remove_if(stations_.begin(), stations_.end(),
                                   [this](const Station& s) { return !me_.empty() && s.call == me_; }),
                    stations_.end());
    version_++;
}

void Stations::setHome(const std::string& locator)
{
    home_ = globus::tidyLocator(locator);
    version_++;
}

void Stations::clear()
{
    stations_.clear();
    if (!partner_.empty()) touch(partner_, 0);
    version_++;
}

void Stations::setPartner(const std::string& call, bool connected, uint64_t nowMs)
{
    std::string normal = protocol::normalCallsign(call);
    if (normal == me_) normal.clear();
    if (normal == partner_ && connected == partnerConnected_) return;
    partner_ = normal;
    partnerConnected_ = connected;
    if (!partner_.empty() && find(partner_) == nullptr) touch(partner_, nowMs);
    version_++;
}

const Station* Stations::find(const std::string& call) const
{
    std::string normal = protocol::normalCallsign(call);
    for (const Station& s : stations_)
    {
        if (s.call == normal) return &s;
    }
    return nullptr;
}

Station& Stations::touch(const std::string& call, uint64_t nowMs)
{
    version_++;
    for (Station& s : stations_)
    {
        if (s.call == call)
        {
            s.lastHeardMs = std::max(s.lastHeardMs, nowMs);
            return s;
        }
    }
    Station& s = stations_.emplace_back();
    s.call = call;
    s.firstHeardMs = nowMs;
    s.lastHeardMs = nowMs;
    return s;
}

void Stations::heard(const std::string& call, uint64_t nowMs, std::optional<double> snr)
{
    std::string normal = protocol::normalCallsign(call);
    if (normal.empty() || normal == me_) return;
    Station& s = touch(normal, nowMs);
    if (!snr || !std::isfinite(*snr)) return;
    // A path heard long ago has changed since.
    if (s.snrDb && nowMs > s.snrMs + PATH_WINDOW_MS) s.path.clear();
    s.snrDb = snr;
    s.snrMs = nowMs;
    s.path.record(*snr);
}

std::optional<PathReport> Stations::weakestPath(uint64_t nowMs) const
{
    std::optional<PathReport> weakest;
    for (const Station& s : stations_)
    {
        if (!s.snrDb || nowMs > s.snrMs + PATH_WINDOW_MS) continue;
        std::optional<double> dB = s.path.average();
        if (!dB || (weakest && *dB >= weakest->dB)) continue;
        weakest = PathReport{s.call, *dB, s.path.count()};
    }
    return weakest;
}

void Stations::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& call = record.header.callsign;
    if (call.empty() || call == me_) return;
    Station& s = touch(call, nowMs);

    if (const protocol::Hello* hello = record.as<protocol::Hello>())
    {
        s.helloHeard = true;
        s.acceptsSessions = hello->acceptsSessions;
        s.automatic = hello->automatic;
        s.takes = hello->takes;
        std::string locator = globus::tidyLocator(hello->locator);
        if (!locator.empty()) s.locator = locator;
        s.software = hello->software;
        s.lastTrafficMs = nowMs;
    }
    else if (const protocol::Status* status = record.as<protocol::Status>())
    {
        s.statusHeard = true;
        s.state = status->state;
        s.partner = status->state == protocol::Status::State::Qrl ? status->partner : std::string();
        s.statusMs = nowMs;
    }
    else
    {
        s.lastTrafficMs = nowMs;
        if (record.as<protocol::Text>() != nullptr) s.texts++;
    }
}

//-------------------------------------------------------------------------
// What the columns show
//-------------------------------------------------------------------------

StatusView Stations::status(const Station& s, uint64_t nowMs) const
{
    using State = protocol::Status::State;
    StatusView view;
    if (!partner_.empty() && s.call == partner_)
    {
        view.word = partnerConnected_ ? "In session with you" : "Calling";
        view.how = partnerConnected_ ? "Our own session with " + s.call : "We're calling " + s.call;
        return view;
    }
    std::string ago = s.statusHeard ? heardAgo(s.statusMs, nowMs) : std::string();
    if (s.statusHeard && s.state == State::Qrl)
    {
        if (nowMs > s.statusMs && nowMs - s.statusMs > QRL_STALE_MS)
        {
            uint64_t min = (nowMs - s.statusMs) / 60000;
            view.word = "QRL? " + std::to_string(min) + " min ago";
            view.filled = false;
            view.how = "From " + s.call + "'s QRL " + ago + ", nothing since: probably free again";
        }
        else
        {
            view.word = s.partner.empty() ? "QRL" : "QRL with " + s.partner;
            view.how = "From " + s.call + "'s QRL, " + ago;
        }
        return view;
    }
    if (s.statusHeard && s.state == State::Qrt && s.lastTrafficMs <= s.statusMs)
    {
        view.word = "QRT";
        view.filled = false;
        view.how = "From " + s.call + "'s QRT, " + ago;
        return view;
    }
    if (s.helloHeard && !s.acceptsSessions)
    {
        view.word = "No sessions";
        view.filled = false;
        view.how = s.call + "'s check-in says it isn't taking sessions";
        return view;
    }
    view.word = "QRV";
    if (s.statusHeard && s.state == State::Qrv)
        view.how = "From " + s.call + "'s QRV, " + ago;
    else
        view.how = "Heard on the D2G group " + heardAgo(s.lastHeardMs, nowMs) + ", with no QRL since";
    return view;
}

std::optional<double> Stations::distanceKm(const Station& s) const
{
    globus::LatLon from, to;
    if (!globus::locatorCentre(home_, from) || !globus::locatorCentre(s.locator, to)) return std::nullopt;
    return globus::distanceKm(from, to);
}

std::optional<double> Stations::bearingDegrees(const Station& s) const
{
    globus::LatLon from, to;
    if (!globus::locatorCentre(home_, from) || !globus::locatorCentre(s.locator, to)) return std::nullopt;
    return globus::bearingDegrees(from, to);
}

std::string Stations::cell(const Station& s, Column column, uint64_t nowMs) const
{
    switch (column)
    {
        case Column::Call:
            return s.call;
        case Column::Status:
        {
            StatusView view = status(s, nowMs);
            return std::string(view.filled ? DOT_FILLED : DOT_HOLLOW) + " " + view.word;
        }
        case Column::Heard:
            return heardAgo(s.lastHeardMs, nowMs);
        case Column::Locator:
            return s.locator;
        case Column::Distance:
        {
            std::optional<double> km = distanceKm(s);
            return km ? globus::withThousands(std::lround(*km)) + " km" : std::string();
        }
        case Column::Bearing:
        {
            std::optional<double> deg = bearingDegrees(s);
            if (!deg) return std::string();
            long rounded = std::lround(*deg) % 360;
            return std::to_string(rounded) + "\xc2\xb0";
        }
        case Column::Key:
            return std::string();
        case Column::Services:
            return s.automatic ? "automatic" : std::string();
        case Column::Snr:
            return s.snrDb ? gearbox::formatDb(*s.snrDb) : std::string();
    }
    return std::string();
}

std::string Stations::whereLine(const Station& s) const
{
    if (s.locator.empty()) return "no locator";
    std::string out = s.locator;
    std::optional<double> km = distanceKm(s);
    if (km) out += " \xc2\xb7 " + globus::withThousands(std::lround(*km)) + " km";
    return out;
}

std::vector<const Station*> Stations::sorted(Column column, bool ascending, uint64_t nowMs) const
{
    std::vector<const Station*> out;
    out.reserve(stations_.size());
    for (const Station& s : stations_) out.push_back(&s);

    // Each column's key: missing values sort last whichever way, and ties
    // fall back to the newest heard.
    auto compare = [&](const Station* a, const Station* b) -> int {
        auto numbers = [](std::optional<double> x, std::optional<double> y) {
            if (!x || !y) return 0;
            return *x < *y ? -1 : (*x > *y ? 1 : 0);
        };
        switch (column)
        {
            case Column::Snr:
                return numbers(a->snrDb, b->snrDb);
            case Column::Call:
            case Column::Key:
                return a->call.compare(b->call);
            case Column::Status:
                return statusRank(status(*a, nowMs)) - statusRank(status(*b, nowMs));
            case Column::Heard:
                return a->lastHeardMs > b->lastHeardMs ? -1 : (a->lastHeardMs < b->lastHeardMs ? 1 : 0);
            case Column::Locator:
                return a->locator.compare(b->locator);
            case Column::Distance:
                return numbers(distanceKm(*a), distanceKm(*b));
            case Column::Bearing:
                return numbers(bearingDegrees(*a), bearingDegrees(*b));
            case Column::Services:
                return (int)b->automatic - (int)a->automatic;
        }
        return 0;
    };
    auto missing = [&](const Station* s) {
        switch (column)
        {
            case Column::Locator:
                return s->locator.empty();
            case Column::Distance:
                return !distanceKm(*s).has_value();
            case Column::Bearing:
                return !bearingDegrees(*s).has_value();
            case Column::Services:
                return !s->automatic;
            case Column::Snr:
                return !s->snrDb.has_value();
            default:
                return false;
        }
    };

    std::stable_sort(out.begin(), out.end(), [&](const Station* a, const Station* b) {
        // The session partner is pinned at the top.
        bool pa = !partner_.empty() && a->call == partner_, pb = !partner_.empty() && b->call == partner_;
        if (pa != pb) return pa;
        bool ma = missing(a), mb = missing(b);
        if (ma != mb) return mb;
        int c = ma ? 0 : compare(a, b);
        if (c != 0) return ascending ? c < 0 : c > 0;
        if (a->lastHeardMs != b->lastHeardMs) return a->lastHeardMs > b->lastHeardMs;
        return a->call < b->call;
    });
    return out;
}

} // namespace broadcast
} // namespace ragtime
