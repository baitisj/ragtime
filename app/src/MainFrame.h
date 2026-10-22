#ifndef RAGTIME_MAIN_FRAME_H
#define RAGTIME_MAIN_FRAME_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "Prefs.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/files/FileTraffic.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/session/SessionChat.h"
#include "ragtime/session/SessionLog.h"

class BroadcastPage;
struct FileCard;
class ConnectionBar;
class FilesPage;
class GlobusPanel;
class GlobusToggle;
class HostConnection;
class SessionPage;
class StationList;
class wxMenu;
class wxNotebook;
class wxPanel;
class wxSplitterWindow;

// The one main window (docs/MOCKUPS.md): the connection bar on top; the
// station list, the tabs (Broadcast, Session, Files) and Globus side by
// side behind draggable dividers it remembers; the status bar at the foot.
// With Globus off the messages take the full width, the station list
// becomes a table, and the Globus toggle moves to the right end of the tab
// strip, unlit.
class MainFrame : public wxFrame
{
public:
    MainFrame();
    ~MainFrame() override;

private:
    void buildMenus();
    void buildLayout();

    // The operator's settings, to the host link, the chat and the window;
    // reconnecting when the callsign or the host changed.
    void applyPrefs(bool reconnect);
    void showSettings();

    void onHostEvents(const std::vector<ragtime::link::HostLink::Event>& events);
    void onClock();
    void showConnection();
    void showStatus();
    void refreshMessages();
    void refreshStations();
    void stationPicked(const std::string& call);
    void stationMenu(const std::string& call);
    void updateTabLabels();

    // Sessions (docs/PLAN.md, "Sometimes we ragchew"): entered only with
    // Call or by being called, left only with Leave or Leave now.
    void callStation(const std::string& call);
    void callPrompt();
    void leaveSession();
    void leaveSessionNow();
    void onSessionData(const uint8_t* bytes, size_t length);
    void onSessionEvents();
    void refreshSession();
    bool canCall() const;

    // Files (docs/PLAN.md, "File transfer"). In a session they go to the
    // partner; to another station, Ragtime offers to call it first: there
    // is no hidden session for a file.
    void sendFilePrompt(const std::string& call);
    void queueFiles(const std::string& call, const std::vector<std::string>& paths);
    void fileAction(const std::string& action, uint64_t id);
    void saveOffer(uint64_t id, bool ask);
    // "Please send FRED.TXT again": a line in the session, now if one is
    // up with the station, or once a call to it connects.
    void askAgain(const std::string& call, const std::string& name, uint64_t kept, uint64_t size);
    void sessionFileMenu(const ragtime::files::FileRow& row, wxMenu& menu);
    void finishedFileMenu(const ragtime::files::FinishedFile& file, wxMenu& menu);
    void onFileEvents();
    void refreshFiles();
    void applyFilePrefs();
    void loadFilesLog();
    void saveFilesLog(bool force);
    // The newest transfer in one line, for the status bar.
    std::string transfersLine() const;

    // Broadcasts.
    bool sendTyped(const std::string& typed);
    std::string broadcastMode() const;
    void checkIn();
    void setPaused(bool paused);

    // Broadcast files (docs/STAGE1.md, package H): Broadcast a file... with
    // the estimate box, each file's card on the Broadcast tab, what its
    // links do, and keeping the seal chain across restarts.
    void broadcastFile();
    bool fileCard(uint64_t id, FileCard& card);
    void broadcastFileAction(uint64_t id, const std::string& action);
    void broadcastFileMenu(const ragtime::files::FileRow& row, wxMenu& menu);
    void keepRepairsUntil(uint64_t id, uint64_t untilMs);
    void receiveFile(uint64_t id);
    void setAutoReceive(bool on);
    void onBroadcastFiles();
    void saveBroadcastState();

    // The gearshift and the width buttons.
    void loadGearshift();
    void showGearshift();
    void gearChanged();
    void showGear();
    void setNarrow(bool narrow);

    // Globus on or off, and the toggle on the tab strip.
    void setGlobus(bool on);
    void placeTabToggle();
    void rememberDividers();

    StationPrefs prefs_;
    ragtime::link::HostLink link_;
    ragtime::broadcast::BroadcastChat chat_;
    ragtime::broadcast::Seals seals_;
    ragtime::broadcast::BroadcastFiles broadcastFiles_;
    ragtime::session::SessionChat session_;
    ragtime::session::SessionLog sessionLog_;
    ragtime::broadcast::Stations stations_;
    ragtime::gearbox::Gearshift gearshift_;
    std::unique_ptr<HostConnection> host_;
    // Session files: the engine, the log of finished transfers (the Files
    // tab's Completed pane, shared with broadcast files), and the two
    // plugged into the session.
    ragtime::files::SessionFiles sessionFiles_;
    ragtime::files::FileHistory history_;
    ragtime::files::FileTraffic fileTraffic_;

    ConnectionBar* bar_ = nullptr;
    wxSplitterWindow* outer_ = nullptr;     // station list | the rest
    wxSplitterWindow* inner_ = nullptr;     // the tabs | Globus
    StationList* stationList_ = nullptr;
    wxPanel* middle_ = nullptr;
    wxNotebook* tabs_ = nullptr;
    BroadcastPage* broadcast_ = nullptr;
    SessionPage* sessionPage_ = nullptr;
    FilesPage* filesPage_ = nullptr;
    GlobusPanel* globus_ = nullptr;
    GlobusToggle* tabToggle_ = nullptr;
    wxMenu* viewMenu_ = nullptr;
    wxTimer clock_;

    bool globusOn_ = true;
    int listWidth_ = 300;       // the station list beside Globus
    int tableWidth_ = 640;      // the station table, Globus off
    int globusWidth_ = 320;

    uint64_t seenMessage_ = 0;  // the newest message id the operator has seen
    size_t unread_ = 0;
    size_t sessionUnread_ = 0;  // heard in the session while its tab is out of sight
    std::string pendingCall_;   // to call once the session we're leaving has ended
    uint64_t stationsVersion_ = 0;
    std::string modeUsed_;      // the mode the host last sent in
    std::string note_;          // the latest thing worth a line in the status bar

    uint64_t broadcastFilesVersion_ = 0;
    uint16_t savedSeal_ = 0;    // the seal chain's number last saved
    std::map<std::string, std::string> askOnConnect_;   // a line to send once a session with the station is up
    std::vector<std::string> logOther_;                 // lines of the files log this version doesn't read
    uint64_t savedLog_ = ~0ull;                         // what the log on disk has (history and parts' changes)
};

#endif
