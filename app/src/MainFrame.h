#ifndef RAGTIME_MAIN_FRAME_H
#define RAGTIME_MAIN_FRAME_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "Pictures.h"
#include "Prefs.h"
#include "SettingsDialog.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastLog.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/broadcast/WatchList.h"
#include "ragtime/ear/FilterControl.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/files/FileTraffic.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/radio/AutoAlc.h"
#include "ragtime/radio/PathLog.h"
#include "ragtime/radio/RecentWork.h"
#include "ragtime/session/SessionChat.h"
#include "ragtime/session/SessionLog.h"

class BroadcastPage;
struct FileCard;
class ConnectionBar;
class EarFrame;
class FilesPage;
class GlobusPanel;
class GlobusToggle;
class HostConnection;
class PictureFrame;
class RecentWorkPane;
class RigControl;
class SessionPage;
class StationList;
class wxBitmapButton;
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
    void showSettings(SettingsDialog::Page page = SettingsDialog::Page::Station);

    void onHostEvents(const std::vector<ragtime::link::HostLink::Event>& events);
    void onClock();
    void showConnection();
    void sizeStatusFields();
    void showThemeButton();     // its icon and tooltip, for the theme showing
    void showStatus();
    void refreshMessages();
    // A station reacted to one of our lines: a line in the status bar for a
    // while, and one more unread on the tab out of sight.
    void noteReaction(const std::string& from, uint8_t code, const std::string& line);
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
    void queueFiles(const std::string& call, const std::vector<std::string>& paths, bool picture = false);
    void fileAction(const std::string& action, uint64_t id);
    void saveOffer(uint64_t id, bool ask);
    // "Please send FRED.TXT again": a line in the session, now if one is
    // up with the station, or once a call to it connects.
    void askAgain(const std::string& call, const std::string& name, uint64_t kept, uint64_t size);
    // Queues the file for the station again, calling it if no session
    // with it is up; a far end with part of it carries on from there.
    // A picture we sent (kept in sentPicturesDir()) goes again as one.
    void sendAgain(const std::string& peer, const std::string& path);
    bool sentPicture(const std::string& path) const;
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
    // Ping a station (BroadcastChat::ping), in the broadcast gear; why not,
    // when it can't go or wouldn't be answered ("in a session"), or "".
    void pingStation(const std::string& call);
    std::string whyNoPing(const std::string& call) const;
    void setPaused(bool paused);

    // Broadcast files (docs/STAGE1.md, package H): Broadcast a file... with
    // the estimate box, each file's card on the Broadcast tab, what its
    // links do, and keeping the seal chain across restarts.
    // Asks which file when path is empty.
    void broadcastFile(wxString path = wxString(), bool picture = false);
    bool fileCard(uint64_t id, FileCard& card);
    void broadcastFileAction(uint64_t id, const std::string& action);
    void broadcastFileMenu(const ragtime::files::FileRow& row, wxMenu& menu);
    void keepRepairsUntil(uint64_t id, uint64_t untilMs, bool reopen = false);
    void reopenRepairs(uint64_t id, uint64_t untilMs);
    void repairGearMenu(const ragtime::broadcast::BroadcastFile& f, FileCard& card);
    void rejectFile(uint64_t id);
    void askAgainOnBroadcast(uint64_t id);
    void receiveFile(uint64_t id);
    void setAutoReceive(bool on);
    void onBroadcastFiles();
    void saveBroadcastState();

    // Inline pictures (docs/STAGE1.md, "Inline pictures"): the picture
    // dialog, to D2G or to the session's partner, the picture kept as it
    // went in sentPicturesDir(); each picture shown among the messages as
    // it stands, on the shelf; and the picture window, one a picture.
    void sendPicture(bool broadcast);
    void updateBroadcastPictures(uint64_t nowMs);
    void updateSessionPictures(uint64_t nowMs);
    wxString pictureHtml(const std::string& key, const wxWindow* list, const wxString& href);
    void viewPicture(const std::string& key, const wxString& title, const std::string& name);
    void viewPictureFile(const std::string& path, const wxString& title);
    void showInFrame(const std::string& key);

    // The gearshift, its button and the 500 Hz switch.
    void loadGearshift();
    void showGearshift();
    // The receive filter's width, for the gearshift's S-meter axis.
    std::optional<double> filterWidthHz() const;
    void gearChanged();
    void showGear();
    // Auto's path S/N: the weakest station heard lately (Stations::weakestPath).
    void updatePathSnr(uint64_t nowMs);
    void setNarrow(bool narrow);

    // The radio (docs/STAGE1.md, "The dial, the path log and the watch
    // list"): its dial read from rigctld (never set), sending held
    // outside the US data segments when the operator asks, a line in the
    // path log for every burst heard, and the watch list's bells.
    void applyRadioPrefs();
    void onDial();
    void showDial();
    // The rig's transmit meters (Hamlib only) on the Auto ALC meter, and
    // a line in the flight recorder for each burst.
    void onMeters();
    void showMeters();
    // Auto ALC and data2g-host's transmit level (TXPOWER).
    void setAutoAlc(bool on);
    void sendTxLevel(double dB, const std::string& why);
    void showAutoAlc();
    // The SWR trip (Settings > Rig): ABORT to the host and sending held
    // until the operator clears it (the SWR bar, or the Settings menu).
    void onSwrTrip(const std::string& what);
    void clearSwrTrip();
    // Why sending is held (receive only), or "" when it isn't.
    std::string sendRefusal() const;
    void applySendGuard();
    // Pause sending (the operator's) and the guard's hold, to the link,
    // the session and the pages.
    void applySending();
    void logPath(const std::string& call, std::optional<double> snr, bool session, uint64_t nowMs);
    // A contact for the path log: a session came up, or a message went
    // between the station and us.
    void logWorked(const std::string& call, uint64_t nowMs);
    // A Text heard on D2G addressed to our call.
    bool addressedToUs(const ragtime::protocol::Record& record) const;
    // Recent work again: on a new path log line (at most every few
    // seconds) and twice a minute for the hour and on now; at once with
    // force.
    void refreshRecent(bool force = false);
    // The station picked in the station list or in Recent work (only one
    // has a pick at a time), or "".
    std::string pickedCall() const;
    void ringBells();
    void showWatchList();
    void showPaths(const std::string& call);
    const ragtime::radio::PathStrip& stripFor(const std::string& call, uint64_t nowMs);

    // Globus on or off, and the toggle on the tab strip.
    void setGlobus(bool on);
    // The Ear, opened or closed (the ear button, View > Ear).
    void setEar(bool open);
    // The Ear sets the rig's filter (RigControl writes only with the box
    // ticked), noted in the flight recorder with why.
    void setRigFilter(const ragtime::radio::RigFilter& filter, const std::string& why);
    void placeTabToggle();
    void rememberDividers();

    StationPrefs prefs_;
    ragtime::link::HostLink link_;
    ragtime::broadcast::BroadcastChat chat_;
    ragtime::broadcast::Seals seals_;
    ragtime::broadcast::BroadcastFiles broadcastFiles_;
    ragtime::session::SessionChat session_;
    ragtime::session::SessionLog sessionLog_;
    ragtime::broadcast::BroadcastLog broadcastLog_;
    ragtime::broadcast::Stations stations_;
    ragtime::gearbox::Gearshift gearshift_;
    std::unique_ptr<HostConnection> host_;
    std::unique_ptr<RigControl> rig_;
    ragtime::radio::PathLog pathLog_;
    ragtime::broadcast::WatchList watch_;
    // Session files: the engine, the log of finished transfers (the Files
    // tab's Completed pane, shared with broadcast files), and the two
    // plugged into the session.
    ragtime::files::SessionFiles sessionFiles_;
    ragtime::files::FileHistory history_;
    ragtime::files::FileTraffic fileTraffic_;

    ConnectionBar* bar_ = nullptr;
    EarFrame* ear_ = nullptr;               // while the Ear is open
    PictureShelf pictures_;
    std::map<std::string, PictureFrame*> pictureFrames_;   // open, by shelf key
    wxSplitterWindow* outer_ = nullptr;     // station list | the rest
    wxSplitterWindow* left_ = nullptr;      // station list over Recent work
    wxSplitterWindow* inner_ = nullptr;     // the tabs | Globus
    StationList* stationList_ = nullptr;
    RecentWorkPane* recent_ = nullptr;
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
    int recentHeight_ = 220;    // Recent work, under the station list

    uint64_t seenMessage_ = 0;  // the newest message id the operator has seen
    size_t unread_ = 0;
    size_t sessionUnread_ = 0;  // heard in the session while its tab is out of sight
    size_t reactionsUnread_ = 0;    // reactions to our lines heard while the Broadcast tab is out of sight
    std::string reactionNote_;      // the latest, shown over the transfers line until reactionNoteUntilMs_
    uint64_t reactionNoteUntilMs_ = 0;
    std::string pendingCall_;   // to call once the session we're leaving has ended
    uint64_t stationsVersion_ = 0;
    std::string modeUsed_;      // the mode the host last sent in
    std::string note_;          // the latest thing worth a line in the status bar
    wxString hostText_;         // the status bar's host field, as shown
    wxString hostTip_;          // and its tooltip
    wxString channelTip_;       // the channel field's tooltip: what is going out
    wxBitmapButton* themeButton_ = nullptr;     // light and dark, where the build can switch

    uint64_t broadcastFilesVersion_ = 0;

    bool paused_ = false;       // Pause sending, the operator's
    bool held_ = false;         // receive only: outside the US data segments, or the dial not known
    std::string heldWhy_;
    bool acceptWanted_ = true;  // Accept sessions, the operator's (LISTEN OFF while held)
    ragtime::radio::PathStrip strip_;   // the picked station's, from the path log
    uint64_t stripVersion_ = ~0ull;
    uint64_t stripMs_ = 0;
    uint64_t trimmedMs_ = 0;    // the path log's memory last cut back
    uint64_t notedBursts_ = 0;  // transmit bursts written to the flight recorder
    // The Ear's filter change and what the rig had (kept here, so quitting
    // puts the rig back after the Ear has closed).
    ragtime::ear::FilterEdit filterEdit_;
    std::string notedFilter_;   // the rig's filter, as last written there
    ragtime::radio::AutoAlc autoAlc_;   // the TX level, kept under the rig's ALC
    ragtime::radio::WorkOrder recentOrder_ = ragtime::radio::WorkOrder::LikelyNow;
    uint64_t recentVersion_ = ~0ull;    // the path log's version Recent work was drawn from
    uint64_t recentMs_ = 0;
    std::string recentBand_;

    uint64_t sealsVersion_ = 0;
    uint16_t savedSeal_ = 0;    // the seal chain's number last saved
    std::map<std::string, std::string> askOnConnect_;   // a line to send once a session with the station is up
    std::vector<std::string> logOther_;                 // lines of the files log this version doesn't read
    uint64_t savedLog_ = ~0ull;                         // what the log on disk has (history and parts' changes)
};

#endif
