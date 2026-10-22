#include "Prefs.h"

#include <optional>

#include <wx/config.h>
#include <wx/tokenzr.h>

using namespace ragtime;

namespace
{

wxConfigBase& config()
{
    return *wxConfigBase::Get();
}

std::string readString(const char* key, const std::string& fallback)
{
    wxString value;
    if (!config().Read(key, &value)) return fallback;
    return std::string(value.utf8_str());
}

} // namespace

StationPrefs loadStationPrefs()
{
    StationPrefs p;
    p.callsign = readString("/Station/Callsign", p.callsign);
    p.locator = readString("/Station/Locator", p.locator);
    p.host = readString("/Connection/Host", p.host);
    p.kissPort = (int)config().ReadLong("/Connection/KissPort", p.kissPort);
    p.commandPort = (int)config().ReadLong("/Connection/CommandPort", p.commandPort);
    return p;
}

void saveStationPrefs(const StationPrefs& p)
{
    config().Write("/Station/Callsign", wxString::FromUTF8(p.callsign));
    config().Write("/Station/Locator", wxString::FromUTF8(p.locator));
    config().Write("/Connection/Host", wxString::FromUTF8(p.host));
    config().Write("/Connection/KissPort", (long)p.kissPort);
    config().Write("/Connection/CommandPort", (long)p.commandPort);
    config().Flush();
}

bool loadBool(const char* key, bool fallback)
{
    return config().ReadBool(key, fallback);
}

void saveBool(const char* key, bool value)
{
    config().Write(key, value);
}

int loadInt(const char* key, int fallback)
{
    return (int)config().ReadLong(key, fallback);
}

void saveInt(const char* key, int value)
{
    config().Write(key, (long)value);
}

uint32_t loadNewestItem(const std::string& callsign)
{
    if (callsign.empty()) return 0;
    long value = config().ReadLong(wxString::FromUTF8("/Broadcast/Numbering/" + callsign), 0);
    return value > 0 && value <= 0xFFFFFF ? (uint32_t)value : 0;
}

void saveNewestItem(const std::string& callsign, uint32_t newest)
{
    if (callsign.empty()) return;
    config().Write(wxString::FromUTF8("/Broadcast/Numbering/" + callsign), (long)newest);
    config().Flush();
}

std::vector<broadcast::Column> loadColumns()
{
    wxString saved;
    if (!config().Read("/Stations/Columns", &saved)) return broadcast::defaultColumns();
    std::vector<broadcast::Column> out;
    wxStringTokenizer words(saved, ",");
    while (words.HasMoreTokens())
    {
        broadcast::Column c;
        if (broadcast::columnFromKey(std::string(words.GetNextToken().utf8_str()), c)
            && broadcast::columnAvailable(c))
            out.push_back(c);
    }
    // The call is always there: it names the row.
    bool hasCall = false;
    for (broadcast::Column c : out) hasCall = hasCall || c == broadcast::Column::Call;
    if (!hasCall) out.insert(out.begin(), broadcast::Column::Call);
    return out;
}

void saveColumns(const std::vector<broadcast::Column>& columns)
{
    wxString saved;
    for (broadcast::Column c : columns)
    {
        if (!saved.empty()) saved += ",";
        saved += broadcast::columnKey(c);
    }
    config().Write("/Stations/Columns", saved);
}

void loadGearshift(gearbox::Gearshift& shift)
{
    int width = (int)config().ReadLong("/Gearshift/Width", (long)gearbox::Width::Hz2400);
    int path = (int)config().ReadLong("/Gearshift/Path", (long)gearbox::Path::Calm);
    if (width >= 0 && width < (int)gearbox::ALL_WIDTHS.size()) shift.setWidth(gearbox::ALL_WIDTHS[width]);
    if (path >= 0 && path < gearbox::PATH_COUNT) shift.setPath(gearbox::ALL_PATHS[path]);
    int gear = (int)config().ReadLong("/Gearshift/Gear", 0);
    if (gear > 0) shift.selectGear(gear);
    shift.setAuto(config().ReadBool("/Gearshift/Auto", false));
}

void saveGearshift(const gearbox::Gearshift& shift)
{
    config().Write("/Gearshift/Width", (long)shift.width());
    config().Write("/Gearshift/Path", (long)shift.path());
    config().Write("/Gearshift/Auto", shift.isAuto());
    std::optional<gearbox::Gear> chosen = shift.chosen();
    config().Write("/Gearshift/Gear", (long)(chosen ? chosen->number : 0));
    config().Flush();
}
