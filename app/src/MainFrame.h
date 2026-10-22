#ifndef RAGTIME_MAIN_FRAME_H
#define RAGTIME_MAIN_FRAME_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "Prefs.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/gearbox/Gearbox.h"
#include "ragtime/link/HostLink.h"

class BroadcastPage;
class ConnectionBar;
class GlobusPanel;
class GlobusToggle;
class HostConnection;
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

    // Broadcasts.
    bool sendTyped(const std::string& typed);
    std::string broadcastMode() const;
    void checkIn();
    void setPaused(bool paused);

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
    ragtime::broadcast::Stations stations_;
    ragtime::gearbox::Gearshift gearshift_;
    std::unique_ptr<HostConnection> host_;

    ConnectionBar* bar_ = nullptr;
    wxSplitterWindow* outer_ = nullptr;     // station list | the rest
    wxSplitterWindow* inner_ = nullptr;     // the tabs | Globus
    StationList* stationList_ = nullptr;
    wxPanel* middle_ = nullptr;
    wxNotebook* tabs_ = nullptr;
    BroadcastPage* broadcast_ = nullptr;
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
    uint64_t stationsVersion_ = 0;
    std::string modeUsed_;      // the mode the host last sent in
    std::string note_;          // the latest thing worth a line in the status bar
};

#endif
