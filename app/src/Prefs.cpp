#include "Prefs.h"

#include <cstdlib>
#include <optional>

#include <wx/config.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include "ragtime/protocol/Fields.h"

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

std::string configPath;

} // namespace

StationPrefs loadStationPrefs()
{
    StationPrefs p;
    p.callsign = readString("/Station/Callsign", p.callsign);
    p.locator = readString("/Station/Locator", p.locator);
    p.host = readString("/Connection/Host", p.host);
    p.kissPort = (int)config().ReadLong("/Connection/KissPort", p.kissPort);
    p.commandPort = (int)config().ReadLong("/Connection/CommandPort", p.commandPort);
    p.receivedFolder = receivedFolder();
    p.autoAccept = loadAutoAccept();
    p.sealChat = config().ReadBool("/Broadcast/SealChat", p.sealChat);
    return p;
}

void saveStationPrefs(const StationPrefs& p)
{
    config().Write("/Station/Callsign", wxString::FromUTF8(p.callsign));
    config().Write("/Station/Locator", wxString::FromUTF8(p.locator));
    config().Write("/Connection/Host", wxString::FromUTF8(p.host));
    config().Write("/Connection/KissPort", (long)p.kissPort);
    config().Write("/Connection/CommandPort", (long)p.commandPort);
    // The default folder stays the default until the operator picks another.
    if (!p.receivedFolder.empty() && p.receivedFolder != receivedFolder()) saveReceivedFolder(p.receivedFolder);
    saveAutoAccept(p.autoAccept);
    config().Write("/Broadcast/SealChat", p.sealChat);
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

std::string receivedFolder()
{
    std::string folder = readString(keys::RECEIVED_FOLDER, "");
    if (!folder.empty()) return folder;
    wxFileName dir = wxFileName::DirName(wxStandardPaths::Get().GetDocumentsDir());
    dir.AppendDir("Ragtime received");
    return std::string(dir.GetPath().utf8_str());
}

void saveReceivedFolder(const std::string& folder)
{
    config().Write(keys::RECEIVED_FOLDER, wxString::FromUTF8(folder));
}

std::vector<std::string> loadAutoAccept()
{
    std::vector<std::string> calls;
    wxStringTokenizer words(wxString::FromUTF8(readString(keys::AUTO_ACCEPT, "")), ", ");
    while (words.HasMoreTokens())
    {
        std::string call = protocol::normalCallsign(std::string(words.GetNextToken().utf8_str()));
        if (!call.empty()) calls.push_back(call);
    }
    return calls;
}

void saveAutoAccept(const std::vector<std::string>& calls)
{
    wxString saved;
    for (const std::string& call : calls)
    {
        if (!saved.empty()) saved += ",";
        saved += wxString::FromUTF8(call);
    }
    config().Write(keys::AUTO_ACCEPT, saved);
}

void setConfigPath(const std::string& path)
{
    configPath = path;
}

std::string appDataDir()
{
#if defined(__UNIX__) && !defined(__WXMAC__)
    // wx's user data folder here is ~/.Ragtime, the very path wxFileConfig
    // keeps the settings file at, so a folder can't be made there. Data
    // goes where XDG says instead: $XDG_DATA_HOME/ragtime, by default
    // ~/.local/share/ragtime.
    wxString base;
    if (!wxGetEnv("XDG_DATA_HOME", &base) || !wxIsAbsolutePath(base))
    {
        wxFileName share = wxFileName::DirName(wxGetHomeDir());
        share.AppendDir(".local");
        share.AppendDir("share");
        base = share.GetPath();
    }
    wxFileName dir = wxFileName::DirName(base);
    dir.AppendDir("ragtime");
    return std::string(dir.GetPath().utf8_str());
#else
    // %APPDATA%\Ragtime, ~/Library/Application Support/Ragtime: the
    // settings live elsewhere (the registry, ~/Library/Preferences).
    return std::string(wxStandardPaths::Get().GetUserDataDir().utf8_str());
#endif
}

std::string filesLogPath()
{
    if (!configPath.empty()) return configPath + ".files";
    wxFileName file(wxString::FromUTF8(appDataDir()), "files.log");
    return std::string(file.GetFullPath().utf8_str());
}

broadcast::Seals::Chain loadSealChain(const std::string& callsign)
{
    broadcast::Seals::Chain chain;
    if (callsign.empty()) return chain;
    wxString saved;
    if (!config().Read(wxString::FromUTF8("/Broadcast/Seals/" + callsign), &saved)) return chain;
    // "<number> <digest in hex>"
    std::string text(saved.utf8_str());
    size_t space = text.find(' ');
    if (space == std::string::npos || text.size() - space - 1 != 2 * protocol::DIGEST_BYTES) return chain;
    long number = std::strtol(text.substr(0, space).c_str(), nullptr, 10);
    if (number <= 0 || number > 0xFFFF) return chain;
    for (size_t i = 0; i < protocol::DIGEST_BYTES; i++)
    {
        std::string pair = text.substr(space + 1 + 2 * i, 2);
        chain.digest[i] = (uint8_t)std::strtoul(pair.c_str(), nullptr, 16);
    }
    chain.number = (uint16_t)number;
    return chain;
}

void saveSealChain(const std::string& callsign, const broadcast::Seals::Chain& chain)
{
    if (callsign.empty() || chain.number == 0) return;
    std::string text = std::to_string(chain.number) + " " + protocol::hex(chain.digest);
    config().Write(wxString::FromUTF8("/Broadcast/Seals/" + callsign), wxString::FromUTF8(text));
    config().Flush();
}
