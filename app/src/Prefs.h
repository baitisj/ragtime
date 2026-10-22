#ifndef RAGTIME_PREFS_H
#define RAGTIME_PREFS_H

#include <cstdint>
#include <string>
#include <vector>

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
// The Files tab's lasting log (finished transfers and kept part files):
// beside the --config file, or in the user data folder.
std::string filesLogPath();

namespace keys
{
constexpr const char* ACCEPT_SESSIONS = "/Connection/AcceptSessions";
constexpr const char* PAUSED = "/Broadcast/Paused";
constexpr const char* GLOBUS = "/Window/Globus";
constexpr const char* LIST_WIDTH = "/Window/StationListWidth";        // beside Globus
constexpr const char* TABLE_WIDTH = "/Window/StationTableWidth";      // Globus off
constexpr const char* GLOBUS_WIDTH = "/Window/GlobusWidth";
constexpr const char* SORT_COLUMN = "/Stations/SortColumn";
constexpr const char* SORT_ASCENDING = "/Stations/SortAscending";
constexpr const char* RECEIVED_FOLDER = "/Files/ReceivedFolder";
constexpr const char* AUTO_ACCEPT = "/Files/AutoAccept";             // callsigns, comma separated
} // namespace keys

#endif
