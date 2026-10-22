#ifndef RAGTIME_PREFS_H
#define RAGTIME_PREFS_H

#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/broadcast/Seals.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/gearbox/Gearbox.h"

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
};

StationPrefs loadStationPrefs();
void saveStationPrefs(const StationPrefs& prefs);

bool loadBool(const char* key, bool fallback);
void saveBool(const char* key, bool value);
int loadInt(const char* key, int fallback);
void saveInt(const char* key, int value);

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

namespace keys
{
constexpr const char* ACCEPT_SESSIONS = "/Connection/AcceptSessions";
constexpr const char* PAUSED = "/Broadcast/Paused";
constexpr const char* NARROW_ONLY = "/Gearshift/NarrowOnly";         // the 500 Hz switch
constexpr const char* GLOBUS = "/Window/Globus";
constexpr const char* LIST_WIDTH = "/Window/StationListWidth";        // beside Globus
constexpr const char* TABLE_WIDTH = "/Window/StationTableWidth";      // Globus off
constexpr const char* GLOBUS_WIDTH = "/Window/GlobusWidth";
constexpr const char* SORT_COLUMN = "/Stations/SortColumn";
constexpr const char* SORT_ASCENDING = "/Stations/SortAscending";
constexpr const char* RECEIVED_FOLDER = "/Files/ReceivedFolder";
constexpr const char* AUTO_ACCEPT = "/Files/AutoAccept";             // callsigns, comma separated
constexpr const char* AUTO_RECEIVE_BROADCAST = "/Files/AutoReceiveBroadcast";   // files on D2G, into the received folder
} // namespace keys

#endif
