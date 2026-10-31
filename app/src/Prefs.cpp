#include "Prefs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <locale>
#include <optional>
#include <sstream>

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

std::string word(const char* key)
{
    return std::string(config().Read(key, wxString()).utf8_str());
}

// Settings > Rig. A file from before the Rig page has only ReadDial.
ragtime::radio::RigSettings loadRig()
{
    using namespace ragtime::radio;
    RigSettings r;
    if (config().HasEntry("/Radio/Source"))
        r.source = sourceFromWord(word("/Radio/Source"));
    else
        r.source = config().ReadBool("/Radio/ReadDial", false) ? RigSource::Rigctld : RigSource::None;
    r.rigctldHost = readString("/Radio/RigctldHost", r.rigctldHost);
    r.rigctldPort = (int)config().ReadLong("/Radio/RigctldPort", r.rigctldPort);
    r.model = (int)config().ReadLong("/Hamlib/Model", r.model);
    r.port = readString("/Hamlib/Port", r.port);
    r.baud = (int)config().ReadLong("/Hamlib/Baud", r.baud);
    r.dataBits = (int)config().ReadLong("/Hamlib/DataBits", r.dataBits);
    r.stopBits = (int)config().ReadLong("/Hamlib/StopBits", r.stopBits);
    r.parity = parityFromWord(word("/Hamlib/Parity"));
    r.handshake = handshakeFromWord(word("/Hamlib/Handshake"));
    r.dtr = lineFromWord(word("/Hamlib/Dtr"));
    r.rts = lineFromWord(word("/Hamlib/Rts"));
    r.backendConf = readString("/Hamlib/Backend", r.backendConf);
    r.pollSeconds = (int)config().ReadLong("/Hamlib/PollSeconds", r.pollSeconds);
    r.pollSeconds = std::min(std::max(r.pollSeconds, POLL_MIN_SECONDS), POLL_MAX_SECONDS);
    r.keyRig = config().ReadBool("/Hamlib/KeyRig", r.keyRig);
    r.swrTrip = config().ReadBool("/Hamlib/SwrTrip", r.swrTrip);
    r.earFilters = config().ReadBool("/Hamlib/EarFilters", r.earFilters);
    r.ptt = pttFromWord(word("/Hamlib/Ptt"));
    r.pttPort = readString("/Hamlib/PttPort", r.pttPort);
    return r;
}

void saveRig(const ragtime::radio::RigSettings& r)
{
    using namespace ragtime::radio;
    config().Write("/Radio/Source", wxString(sourceWord(r.source)));
    // For a Ragtime from before the Rig page.
    config().Write("/Radio/ReadDial", r.source == RigSource::Rigctld);
    config().Write("/Radio/RigctldHost", wxString::FromUTF8(r.rigctldHost));
    config().Write("/Radio/RigctldPort", (long)r.rigctldPort);
    config().Write("/Hamlib/Model", (long)r.model);
    config().Write("/Hamlib/Port", wxString::FromUTF8(r.port));
    config().Write("/Hamlib/Baud", (long)r.baud);
    config().Write("/Hamlib/DataBits", (long)r.dataBits);
    config().Write("/Hamlib/StopBits", (long)r.stopBits);
    config().Write("/Hamlib/Parity", wxString(parityWord(r.parity)));
    config().Write("/Hamlib/Handshake", wxString(handshakeWord(r.handshake)));
    config().Write("/Hamlib/Dtr", wxString(lineWord(r.dtr)));
    config().Write("/Hamlib/Rts", wxString(lineWord(r.rts)));
    config().Write("/Hamlib/Backend", wxString::FromUTF8(r.backendConf));
    config().Write("/Hamlib/PollSeconds", (long)r.pollSeconds);
    config().Write("/Hamlib/KeyRig", r.keyRig);
    config().Write("/Hamlib/SwrTrip", r.swrTrip);
    config().Write("/Hamlib/EarFilters", r.earFilters);
    config().Write("/Hamlib/Ptt", wxString(pttWord(r.ptt)));
    config().Write("/Hamlib/PttPort", wxString::FromUTF8(r.pttPort));
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
    p.receivedFolder = receivedFolder();
    p.autoAccept = loadAutoAccept();
    p.sealChat = config().ReadBool("/Broadcast/SealChat", p.sealChat);
    p.picturesAsTheyCome = config().ReadBool(keys::PICTURES_AS_THEY_COME, p.picturesAsTheyCome);
    p.slowRepairMinutes = (int)config().ReadLong("/Broadcast/SlowRepairMinutes", p.slowRepairMinutes);
    p.slowRepairMinutes = std::min(std::max(p.slowRepairMinutes, SLOW_REPAIR_MIN_MINUTES), SLOW_REPAIR_MAX_MINUTES);
    p.repairFloor = readString("/Broadcast/RepairFloor", p.repairFloor);
    p.repairCap = config().ReadBool("/Broadcast/RepairCap", p.repairCap);
    p.repairCapMinutes = (int)config().ReadLong("/Broadcast/RepairCapMinutes", p.repairCapMinutes);
    p.repairCapMinutes = std::min(std::max(p.repairCapMinutes, REPAIR_CAP_MIN_MINUTES), REPAIR_CAP_MAX_MINUTES);
    p.rig = loadRig();
    p.usSegmentsOnly = config().ReadBool("/Radio/UsSegmentsOnly", p.usSegmentsOnly);
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
    config().Write(keys::PICTURES_AS_THEY_COME, p.picturesAsTheyCome);
    config().Write("/Broadcast/SlowRepairMinutes", (long)p.slowRepairMinutes);
    config().Write("/Broadcast/RepairFloor", wxString::FromUTF8(p.repairFloor));
    config().Write("/Broadcast/RepairCap", p.repairCap);
    config().Write("/Broadcast/RepairCapMinutes", (long)p.repairCapMinutes);
    saveRig(p.rig);
    config().Write("/Radio/UsSegmentsOnly", p.usSegmentsOnly);
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

std::string loadString(const char* key, const std::string& fallback)
{
    return config().Read(key, wxString::FromUTF8(fallback)).ToStdString(wxConvUTF8);
}

void saveString(const char* key, const std::string& value)
{
    config().Write(key, wxString::FromUTF8(value));
}

namespace
{

// A rig or mode as a wxConfig path part: letters, digits, - and _.
std::string pathPart(const std::string& s)
{
    std::string out;
    for (char c : s) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? std::string("_") : out;
}

std::string presetKey(const std::string& rig, const std::string& mode, int n)
{
    return "/Ear/Presets/" + pathPart(rig) + "/" + pathPart(mode) + "/" + std::to_string(n);
}

std::string notchKey(const std::string& rig, const std::string& mode)
{
    return "/Ear/Notch/" + pathPart(rig) + "/" + pathPart(mode);
}

std::string calibrationKey(const std::string& rig, const std::string& mode, int widthHz)
{
    return "/Ear/Calibration/" + pathPart(rig) + "/" + pathPart(mode) + "/" + std::to_string(widthHz);
}

} // namespace

std::optional<ragtime::radio::RigFilter> loadEarPreset(const std::string& rig, const std::string& mode, int n)
{
    // "2400 0.5020 0.4510 1 0.2510": the width, the PBT knobs, the notch
    // on (1) or off (0) and where; a dash for what the rig hasn't, and a
    // preset from before the notch has only the first three. Read and
    // written in the C locale, whatever the desktop's.
    std::istringstream in(loadString(presetKey(rig, mode, n).c_str(), ""));
    in.imbue(std::locale::classic());
    int width = 0;
    std::string knobs[4];
    if (!(in >> width >> knobs[0] >> knobs[1]) || width <= 0) return std::nullopt;
    in >> knobs[2] >> knobs[3];
    ragtime::radio::RigFilter f;
    f.mode = mode;
    f.widthHz = width;
    std::optional<double> notch;
    std::optional<double>* fields[3] = {&f.pbtIn, &f.pbtOut, &notch};
    for (int i = 0; i < 4; i++)
    {
        if (knobs[i].empty() || knobs[i] == "-") continue;
        std::istringstream k(knobs[i]);
        k.imbue(std::locale::classic());
        double v = 0;
        if (!(k >> v)) continue;
        if (i == 3)
            f.notchRaw = v;
        else
            *fields[i] = v;
    }
    if (notch) f.notchOn = *notch != 0;
    return f;
}

void saveEarPreset(const std::string& rig, int n, const ragtime::radio::RigFilter& f)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(4);
    out << f.widthHz;
    const std::optional<double> notch = f.notchOn ? std::optional<double>(*f.notchOn ? 1 : 0) : std::nullopt;
    for (const std::optional<double>& v : {f.pbtIn, f.pbtOut, notch, f.notchRaw})
    {
        out << ' ';
        if (v)
            out << *v;
        else
            out << '-';
    }
    saveString(presetKey(rig, f.mode, n).c_str(), out.str());
    config().Flush();
}

std::optional<ragtime::ear::KnobModel> loadEarCalibration(const std::string& rig, const std::string& mode,
                                                          int widthHz)
{
    // An older single number (Hz per knob alone) reads as none: calibrate again.
    return ragtime::ear::KnobModel::fromText(loadString(calibrationKey(rig, mode, widthHz).c_str(), ""));
}

void saveEarCalibration(const std::string& rig, const std::string& mode, int widthHz,
                        const ragtime::ear::KnobModel& model)
{
    saveString(calibrationKey(rig, mode, widthHz).c_str(), model.text());
    // Half a minute of the operator's band: kept at once.
    config().Flush();
}

ragtime::ear::NotchModel loadEarNotch(const std::string& rig, const std::string& mode)
{
    return ragtime::ear::NotchModel::fromText(loadString(notchKey(rig, mode).c_str(), ""))
        .value_or(ragtime::ear::NotchModel());
}

void saveEarNotch(const std::string& rig, const std::string& mode, const ragtime::ear::NotchModel& model)
{
    saveString(notchKey(rig, mode).c_str(), model.text());
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
        if (broadcast::columnFromKey(std::string(words.GetNextToken().utf8_str()), c)) out.push_back(c);
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
    // Before the 500 Hz switch had its own key it followed the width.
    shift.setNarrowOnly(config().ReadBool(keys::NARROW_ONLY, shift.width() == gearbox::Width::Hz500));
    int gear = (int)config().ReadLong("/Gearshift/Gear", 0);
    wxString mode = config().Read("/Gearshift/Mode", "");
    if (!mode.empty()) shift.selectMode(std::string(mode.utf8_str()));
    else if (gear > 0) shift.selectGear(gear);
    shift.setAuto(config().ReadBool("/Gearshift/Auto", false));
}

void saveGearshift(const gearbox::Gearshift& shift)
{
    config().Write("/Gearshift/Width", (long)shift.width());
    config().Write("/Gearshift/Path", (long)shift.path());
    config().Write("/Gearshift/Auto", shift.isAuto());
    std::optional<gearbox::Gear> chosen = shift.chosen();
    config().Write("/Gearshift/Gear", (long)(chosen ? chosen->number : 0));
    config().Write("/Gearshift/Mode", wxString::FromUTF8(shift.chosenMode()));
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

namespace
{

std::vector<std::string> readCalls(const char* key)
{
    std::vector<std::string> calls;
    wxStringTokenizer words(wxString::FromUTF8(readString(key, "")), ", ");
    while (words.HasMoreTokens())
    {
        std::string call = protocol::normalCallsign(std::string(words.GetNextToken().utf8_str()));
        if (!call.empty()) calls.push_back(call);
    }
    return calls;
}

void writeCalls(const char* key, const std::vector<std::string>& calls)
{
    wxString saved;
    for (const std::string& call : calls)
    {
        if (!saved.empty()) saved += ",";
        saved += wxString::FromUTF8(call);
    }
    config().Write(key, saved);
}

} // namespace

std::vector<std::string> loadAutoAccept()
{
    return readCalls(keys::AUTO_ACCEPT);
}

void saveAutoAccept(const std::vector<std::string>& calls)
{
    writeCalls(keys::AUTO_ACCEPT, calls);
}

std::vector<std::string> loadWatchList()
{
    return readCalls(keys::WATCH_LIST);
}

void saveWatchList(const std::vector<std::string>& calls)
{
    writeCalls(keys::WATCH_LIST, calls);
    config().Flush();
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

std::string sessionLogDir()
{
    if (!configPath.empty()) return configPath + ".sessions";
    wxFileName dir = wxFileName::DirName(wxString::FromUTF8(appDataDir()));
    dir.AppendDir("sessions");
    return std::string(dir.GetPath().utf8_str());
}

std::string broadcastLogPath()
{
    if (!configPath.empty()) return configPath + ".broadcast.log";
    wxFileName file(wxString::FromUTF8(appDataDir()), "broadcast.log");
    return std::string(file.GetFullPath().utf8_str());
}

std::string broadcastPartsDir()
{
    if (!configPath.empty()) return configPath + ".broadcast-parts";
    wxFileName dir = wxFileName::DirName(wxString::FromUTF8(appDataDir()));
    dir.AppendDir("broadcast-parts");
    return std::string(dir.GetPath().utf8_str());
}

std::string pathLogPath()
{
    if (!configPath.empty()) return configPath + ".paths.log";
    wxFileName file(wxString::FromUTF8(appDataDir()), "paths.log");
    return std::string(file.GetFullPath().utf8_str());
}

std::string sentPicturesDir()
{
    if (!configPath.empty()) return configPath + ".sent-pictures";
    wxFileName dir = wxFileName::DirName(wxString::FromUTF8(appDataDir()));
    dir.AppendDir("sent-pictures");
    return std::string(dir.GetPath().utf8_str());
}

std::string sentVoiceDir()
{
    if (!configPath.empty()) return configPath + ".sent-voice";
    wxFileName dir = wxFileName::DirName(wxString::FromUTF8(appDataDir()));
    dir.AppendDir("sent-voice");
    return std::string(dir.GetPath().utf8_str());
}

std::string flightRecorderDir()
{
    if (!configPath.empty()) return configPath + ".flight";
    wxFileName dir = wxFileName::DirName(wxString::FromUTF8(appDataDir()));
    dir.AppendDir("flight");
    return std::string(dir.GetPath().utf8_str());
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
