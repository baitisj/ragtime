#ifndef RAGTIME_PREFS_H
#define RAGTIME_PREFS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/broadcast/Seals.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/ear/FilterControl.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/radio/RigFilter.h"
#include "ragtime/radio/RigSettings.h"

// What Ragtime remembers between runs, in wxConfig (~/.Ragtime on Linux,
// the registry on Windows, or the file given with --config). One place for
// the keys, so the window, the dialogs and the connection agree on them.
struct StationPrefs
{
    std::string callsign;
    std::string locator;
    std::string host = "127.0.0.1";
    int kissPort = 8100;
    int commandPort = 8300;     // the session data port is the next one
    std::string receivedFolder; // where files saved from a session go
    std::vector<std::string> autoAccept;   // stations whose files are saved without asking
    bool sealChat = false;      // our broadcast chat is sealed too (files always are)
    // A repair round may spend this long in gears slower than its own, for
    // stations that hear us weakly (Seals::setSlowResendSeconds).
    int slowRepairMinutes = (int)(ragtime::broadcast::Seals::SLOW_RESEND_SECONDS / 60);
    // Repairs never go slower than this mode (Data2G's name; empty: as
    // slow as an asker needs), and a file's repairs close once they have
    // taken this much airtime (Seals::setRepairFloor, setRepairCapSeconds).
    std::string repairFloor;
    bool repairCap = true;
    int repairCapMinutes = 30;
    // Settings > Rig: the dial, read (never set) from rigctld or from the
    // rig opened with Hamlib, for the path log's bands and the data
    // segment check below; and keying the rig at data2g-host's PTT.
    ragtime::radio::RigSettings rig;
    // Receive only outside the US data segments, and while the dial isn't
    // known (needs a rig).
    bool usSegmentsOnly = false;

    bool readsDial() const { return rig.source != ragtime::radio::RigSource::None; }
};

constexpr int SLOW_REPAIR_MIN_MINUTES = 1;
constexpr int SLOW_REPAIR_MAX_MINUTES = 60;
constexpr int REPAIR_CAP_MIN_MINUTES = 1;
constexpr int REPAIR_CAP_MAX_MINUTES = 240;

StationPrefs loadStationPrefs();
void saveStationPrefs(const StationPrefs& prefs);

bool loadBool(const char* key, bool fallback);
void saveBool(const char* key, bool value);
int loadInt(const char* key, int fallback);
void saveInt(const char* key, int value);
std::string loadString(const char* key, const std::string& fallback);
void saveString(const char* key, const std::string& value);

// The Ear's presets (1 to EAR_PRESETS) and its calibrations of the twin
// PBT, by rig (RigControl::rigKey) and mode, and for calibrations the
// width: under /Ear/Presets and /Ear/Calibration.
constexpr int EAR_PRESETS = 4;
std::optional<ragtime::radio::RigFilter> loadEarPreset(const std::string& rig, const std::string& mode, int n);
void saveEarPreset(const std::string& rig, int n, const ragtime::radio::RigFilter& filter);
// What Calibrate learned, if it has run for this rig, mode and width.
std::optional<ragtime::ear::KnobModel> loadEarCalibration(const std::string& rig, const std::string& mode,
                                                          int widthHz);
void saveEarCalibration(const std::string& rig, const std::string& mode, int widthHz,
                        const ragtime::ear::KnobModel& model);

// The broadcast numbering is kept across restarts, one for each callsign
// (docs/PROTOCOL.md, "Sequence numbers").
uint32_t loadNewestItem(const std::string& callsign);
void saveNewestItem(const std::string& callsign, uint32_t newest);

// Our seal chain on the group (docs/PROTOCOL.md, "Seals and repair"): the
// newest seal's number and digest, kept with the numbering so the next
// seal after a restart names the last one.
ragtime::broadcast::Seals::Chain loadSealChain(const std::string& callsign);
void saveSealChain(const std::string& callsign, const ragtime::broadcast::Seals::Chain& chain);

// The station list's table columns, in order, and its sort.
std::vector<ragtime::broadcast::Column> loadColumns();
void saveColumns(const std::vector<ragtime::broadcast::Column>& columns);

// The gearshift's width, path, Auto and chosen gear.
void loadGearshift(ragtime::gearbox::Gearshift& shift);
void saveGearshift(const ragtime::gearbox::Gearshift& shift);

// Files (docs/PLAN.md, "File transfer"): where received files go (by
// default "Ragtime received" in the documents folder), and the stations
// whose session files are taken without asking.
std::string receivedFolder();
void saveReceivedFolder(const std::string& folder);
std::vector<std::string> loadAutoAccept();
void saveAutoAccept(const std::vector<std::string>& calls);

// The watch list's lasting bells (WatchList::calls()).
std::vector<std::string> loadWatchList();
void saveWatchList(const std::vector<std::string>& calls);

// --config's file, when one was given ("" for the usual place).
void setConfigPath(const std::string& path);
// Where Ragtime keeps its own files (the files log, a thresholds file):
// ~/.local/share/ragtime on Linux ($XDG_DATA_HOME/ragtime when that is
// set), %APPDATA%\Ragtime on Windows. Not wx's user data folder, which on
// Linux is ~/.Ragtime, the settings file itself. It may not exist yet.
std::string appDataDir();
// The Files tab's lasting log (finished transfers and kept part files):
// beside the --config file, or in appDataDir().
std::string filesLogPath();
// The Session tab's lasting log, a file per station: beside the --config
// file (<config>.sessions), or appDataDir()/sessions.
std::string sessionLogDir();
// The Broadcast tab's lasting log: beside the --config file
// (<config>.broadcast.log), or appDataDir()/broadcast.log.
std::string broadcastLogPath();
// The flight recorder's files (docs/FLIGHT_RECORDER.md): beside the
// --config file (<config>.flight), or appDataDir()/flight.
std::string flightRecorderDir();
// Broadcast files not yet whole, a file each, kept a week so a restart can
// still ask for repairs: beside the --config file (<config>.broadcast-parts),
// or appDataDir()/broadcast-parts.
std::string broadcastPartsDir();
// The path log, a line for every burst heard, appended and never cut:
// beside the --config file (<config>.paths.log), or appDataDir()/paths.log.
std::string pathLogPath();

namespace keys
{
constexpr const char* ACCEPT_SESSIONS = "/Connection/AcceptSessions";
constexpr const char* PAUSED = "/Broadcast/Paused";
constexpr const char* NARROW_ONLY = "/Gearshift/NarrowOnly";         // the 500 Hz switch
constexpr const char* GLOBUS = "/Window/Globus";
constexpr const char* LIST_WIDTH = "/Window/StationListWidth";        // beside Globus
constexpr const char* TABLE_WIDTH = "/Window/StationTableWidth";      // Globus off
constexpr const char* GLOBUS_WIDTH = "/Window/GlobusWidth";
constexpr const char* RECENT_HEIGHT = "/Window/RecentWorkHeight";      // under the station list
constexpr const char* THEME = "/Window/Theme";                        // ThemeChoice: 0 the desktop's, 1 light, 2 dark
constexpr const char* SORT_COLUMN = "/Stations/SortColumn";
constexpr const char* SORT_ASCENDING = "/Stations/SortAscending";
constexpr const char* RECENT_ORDER = "/Stations/RecentWorkOrder";     // radio::WorkOrder
constexpr const char* RECEIVED_FOLDER = "/Files/ReceivedFolder";
constexpr const char* AUTO_ACCEPT = "/Files/AutoAccept";             // callsigns, comma separated
constexpr const char* WATCH_LIST = "/Stations/Watch";                // callsigns, comma separated
constexpr const char* AUTO_RECEIVE_BROADCAST = "/Files/AutoReceiveBroadcast";   // files on D2G, into the received folder
constexpr const char* EAR_SOURCE = "/Ear/Source";                    // a PipeWire source's node.name; empty follows data2g-host
constexpr const char* EAR_SPEED = "/Ear/RowsPerSecond";               // the waterfall's rows a second
} // namespace keys

#endif
