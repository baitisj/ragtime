#include "MainFrame.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>

#include <wx/aboutdlg.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/config.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/persist/toplevel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/tglbtn.h>

#include "BroadcastPage.h"
#include "ConnectionBar.h"
#include "GearshiftDialog.h"
#include "HostConnection.h"
#include "SessionPage.h"
#include "SettingsDialog.h"
#include "StationList.h"
#include "Thresholds.h"
#include "TxLamp.h"
#include "globus/GlobusPanel.h"
#include "globus/GlobusToggle.h"
#include "ragtime/Version.h"
#include "ragtime/globus/Sun.h"

using namespace ragtime;
using Event = link::HostLink::Event;

namespace
{

enum
{
    ID_GEARSHIFT = wxID_HIGHEST + 1,
    ID_STATION_SETTINGS,
    ID_NARROW,
    ID_GLOBUS,
    ID_MESSAGE_STATION,
    ID_CHECK_IN,
    ID_PAUSE,
    ID_TAB_BROADCAST,
    ID_TAB_SESSION,
    ID_TAB_FILES,
    ID_STATION_MESSAGE,
    ID_STATION_COPY,
    ID_CALL,
    ID_LEAVE,
    ID_LEAVE_NOW,
    ID_STATION_CALL,
    ID_STATION_LEAVE,
};

enum Tab
{
    TAB_BROADCAST,
    TAB_SESSION,
    TAB_FILES,
};

enum StatusField
{
    FIELD_CHANNEL,
    FIELD_MODE,
    FIELD_TRANSFERS,
};

constexpr int CLOCK_MS = 1000;
constexpr int MIN_PANE = 200;

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

wxPanel* placeholderPage(wxWindow* parent, const wxString& text)
{
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    auto* label = new wxStaticText(page, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    label->Disable();
    sizer->Add(label, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 12);
    sizer->AddStretchSpacer();
    page->SetSizer(sizer);
    return page;
}

std::string software()
{
    return std::string("Ragtime ") + versionString();
}

// Local time's lead on UTC, daylight time in it, for the strip's clock times.
long localOffsetSeconds()
{
    std::time_t now = std::time(nullptr);
    const std::tm* localNow = std::localtime(&now);
    if (localNow == nullptr) return 0;
    std::tm local = *localNow;
    const std::tm* utcNow = std::gmtime(&now);
    if (utcNow == nullptr) return 0;
    return globus::utcOffsetSeconds(local, *utcNow);
}

// "From W1AW's QRL, 3 min ago" as the middle of a sentence.
std::string lowerFirst(std::string text)
{
    if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z' && (text.size() < 2 || !(text[1] >= 'A' && text[1] <= 'Z')))
        text[0] = (char)(text[0] - 'A' + 'a');
    return text;
}

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Ragtime", wxDefaultPosition, wxSize(1280, 720))
    , prefs_(loadStationPrefs())
    , link_({prefs_.callsign, loadBool(keys::ACCEPT_SESSIONS, true), false})
    , chat_(link_)
    , session_(link_)
    , clock_(this)
{
    // Every dialog and the main window must fit Jeff's 1366x768 screen.
    SetMinSize(wxSize(960, 600));

    globusOn_ = loadBool(keys::GLOBUS, true);
    listWidth_ = std::max(MIN_PANE, loadInt(keys::LIST_WIDTH, listWidth_));
    tableWidth_ = std::max(MIN_PANE, loadInt(keys::TABLE_WIDTH, tableWidth_));
    globusWidth_ = std::max(MIN_PANE, loadInt(keys::GLOBUS_WIDTH, globusWidth_));

    buildMenus();
    buildLayout();

    host_ = std::make_unique<HostConnection>(link_, [this](const std::vector<Event>& events) {
        onHostEvents(events);
    });
    HostConnection::DataSide data;
    data.bytes = [this](const uint8_t* bytes, size_t length) { onSessionData(bytes, length); };
    data.take = [this]() { return session_.takeDataOutput(HostConnection::nowMs()); };
    data.up = [this](bool up) {
        session_.dataPort(up, HostConnection::nowMs());
        onSessionEvents();
        refreshSession();
    };
    host_->setDataSide(std::move(data));

    loadGearshift();
    setNarrow(gearshift_.width() == gearbox::Width::Hz500);
    bar_->acceptSessions->SetValue(link_.settings().acceptSessions);
    setPaused(loadBool(keys::PAUSED, false));
    stationList_->setColumns(loadColumns());
    broadcast::Column sort = broadcast::Column::Heard;
    if (int saved = loadInt(keys::SORT_COLUMN, (int)sort); saved >= 0 && saved < broadcast::COLUMN_COUNT)
        sort = broadcast::ALL_COLUMNS[(size_t)saved];
    stationList_->setSort(sort, loadBool(keys::SORT_ASCENDING, true));

    applyPrefs(true);
    showStatus();

    wxPersistentRegisterAndRestore(this, "MainWindow");
    if (!IsMaximized() && GetPosition() == wxDefaultPosition) Centre();

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { onClock(); });
    clock_.Start(CLOCK_MS);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        rememberDividers();
        saveBool(keys::GLOBUS, globusOn_);
        saveInt(keys::LIST_WIDTH, listWidth_);
        saveInt(keys::TABLE_WIDTH, tableWidth_);
        saveInt(keys::GLOBUS_WIDTH, globusWidth_);
        saveColumns(stationList_->columns());
        saveInt(keys::SORT_COLUMN, (int)stationList_->sortColumn());
        saveBool(keys::SORT_ASCENDING, stationList_->sortAscending());
        wxConfigBase::Get()->Flush();
        e.Skip();
    });
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
        if (e.GetActive()) updateTabLabels();
        e.Skip();
    });

    if (prefs_.callsign.empty()) CallAfter([this]() { showSettings(); });
}

MainFrame::~MainFrame()
{
    clock_.Stop();
    host_.reset();
}

//-------------------------------------------------------------------------
// Building the window
//-------------------------------------------------------------------------

void MainFrame::buildMenus()
{
    auto* file = new wxMenu;
    file->Append(wxID_EXIT, "&Quit\tCtrl+Q");

    auto* station = new wxMenu;
    station->Append(ID_MESSAGE_STATION, "&Message the selected station\tCtrl+M",
                    "Starts a line to the station picked in the list, \"@W1AW\"");
    station->Append(ID_CHECK_IN, "Check &in\tCtrl+I", "Broadcasts your check-in: callsign, locator and whether "
                                                       "you take sessions");
    station->AppendSeparator();
    station->Append(ID_CALL, "&Call...\tCtrl+K", "Starts a session (a ragchew) with the station picked in the list, "
                                                "or one you name");
    station->Append(ID_LEAVE, "&Leave the session\tCtrl+L",
                    "Leaves once what you sent is acknowledged (DISCONNECT); while calling, stops the call");
    station->Append(ID_LEAVE_NOW, "Leave &now\tCtrl+Shift+L", "Leaves at once (ABORT)");
    station->AppendSeparator();
    station->AppendCheckItem(ID_PAUSE, "&Pause sending\tCtrl+P");

    viewMenu_ = new wxMenu;
    viewMenu_->AppendCheckItem(ID_GLOBUS, "&Globus\tCtrl+Shift+G");
    auto* columns = new wxMenu;
    for (broadcast::Column c : broadcast::ALL_COLUMNS)
    {
        wxString label = broadcast::columnLabel(c);
        if (!broadcast::columnAvailable(c)) label += " (needs a host report)";
        columns->AppendCheckItem(StationList::ID_COLUMN_FIRST + (int)c, label);
    }
    viewMenu_->AppendSubMenu(columns, "Station &columns",
                             "The station table's columns, shown while Globus is off");
    viewMenu_->AppendSeparator();
    viewMenu_->Append(ID_TAB_BROADCAST, "&Broadcast\tCtrl+1");
    viewMenu_->Append(ID_TAB_SESSION, "&Session\tCtrl+2");
    viewMenu_->Append(ID_TAB_FILES, "&Files\tCtrl+3");

    auto* settings = new wxMenu;
    settings->Append(ID_STATION_SETTINGS, "&Station...\tCtrl+,");
    settings->Append(ID_GEARSHIFT, "&Gearshift...\tCtrl+G");
    settings->AppendCheckItem(ID_NARROW, "&500 Hz only\tCtrl+5");

    auto* help = new wxMenu;
    help->Append(wxID_ABOUT, "&About Ragtime");

    auto* menus = new wxMenuBar;
    menus->Append(file, "&File");
    menus->Append(station, "S&tation");
    menus->Append(viewMenu_, "&View");
    menus->Append(settings, "&Settings");
    menus->Append(help, "&Help");
    SetMenuBar(menus);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { showGearshift(); }, ID_GEARSHIFT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { showSettings(); }, ID_STATION_SETTINGS);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setNarrow(e.IsChecked()); }, ID_NARROW);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setGlobus(e.IsChecked()); }, ID_GLOBUS);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setPaused(e.IsChecked()); }, ID_PAUSE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { checkIn(); }, ID_CHECK_IN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { callPrompt(); }, ID_CALL);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { leaveSession(); }, ID_LEAVE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { leaveSessionNow(); }, ID_LEAVE_NOW);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(canCall() || session_.active()); }, ID_CALL);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(session_.phase() == session::SessionChat::Phase::Calling
                 || session_.phase() == session::SessionChat::Phase::Connected);
    }, ID_LEAVE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(session_.active()); }, ID_LEAVE_NOW);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        tabs_->SetSelection(TAB_BROADCAST);
        if (!stationList_->selected().empty()) broadcast_->address(stationList_->selected());
    }, ID_MESSAGE_STATION);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { tabs_->SetSelection(TAB_BROADCAST); }, ID_TAB_BROADCAST);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { tabs_->SetSelection(TAB_SESSION); }, ID_TAB_SESSION);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { tabs_->SetSelection(TAB_FILES); }, ID_TAB_FILES);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { stationList_->toggleColumn(e.GetId()); },
         StationList::ID_COLUMN_FIRST, StationList::ID_COLUMN_FIRST + broadcast::COLUMN_COUNT - 1);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        int index = e.GetId() - StationList::ID_COLUMN_FIRST;
        broadcast::Column c = broadcast::ALL_COLUMNS[(size_t)index];
        const auto& shown = stationList_->columns();
        e.Check(std::find(shown.begin(), shown.end(), c) != shown.end());
        e.Enable(broadcast::columnAvailable(c) && c != broadcast::Column::Call);
    }, StationList::ID_COLUMN_FIRST, StationList::ID_COLUMN_FIRST + broadcast::COLUMN_COUNT - 1);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(!stationList_->selected().empty()); },
         ID_MESSAGE_STATION);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(!prefs_.callsign.empty()); }, ID_CHECK_IN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxAboutDialogInfo info;
        info.SetName("Ragtime");
        info.SetVersion(versionString());
        info.SetDescription("A plain chat and file-transfer client for Data2G.\n"
                            "Welcome to the D2G net. Sometimes we ragchew.\n\n"
                            "Ragtime talks to a data2g-host you run; it never starts one.");
        info.SetLicence("MIT");
        info.SetWebSite("https://github.com/baitisj/ragtime");
        wxAboutBox(info, this);
    }, wxID_ABOUT);
}

void MainFrame::buildLayout()
{
    auto* root = new wxPanel(this);
    bar_ = new ConnectionBar(root);

    outer_ = new wxSplitterWindow(root, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxSP_LIVE_UPDATE | wxSP_3DSASH | wxSP_NO_XP_THEME);
    outer_->SetMinimumPaneSize(MIN_PANE);
    outer_->SetSashGravity(0.0);
    stationList_ = new StationList(outer_, stations_);

    inner_ = new wxSplitterWindow(outer_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxSP_LIVE_UPDATE | wxSP_3DSASH | wxSP_NO_XP_THEME);
    inner_->SetMinimumPaneSize(MIN_PANE);
    // Globus keeps its width as the window grows; the messages take the rest.
    inner_->SetSashGravity(1.0);

    middle_ = new wxPanel(inner_);
    tabs_ = new wxNotebook(middle_, wxID_ANY);
    broadcast_ = new BroadcastPage(tabs_, chat_);
    // Tab order is fixed: Broadcast, Session, Files (CLAUDE.md).
    tabs_->AddPage(broadcast_, "Broadcast", true);
    sessionPage_ = new SessionPage(tabs_, session_);
    tabs_->AddPage(sessionPage_, wxString::FromUTF8("Session \xc2\xb7 none"));
    tabs_->AddPage(placeholderPage(tabs_, "No files yet."), "Files");
    auto* middleSizer = new wxBoxSizer(wxVERTICAL);
    middleSizer->Add(tabs_, 1, wxEXPAND);
    middle_->SetSizer(middleSizer);
    // The Globus toggle's place on the tab strip while the globe is hidden:
    // over the strip's empty right end, unlit.
    tabToggle_ = new GlobusToggle(middle_);
    tabToggle_->setOn(false);
    tabToggle_->Hide();

    globus_ = new GlobusPanel(inner_);
    outer_->SplitVertically(stationList_, inner_, globusOn_ ? listWidth_ : tableWidth_);
    if (globusOn_)
        inner_->SplitVertically(middle_, globus_, -globusWidth_);
    else
        inner_->Initialize(middle_);
    if (!globusOn_) globus_->Hide();

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(bar_, 0, wxEXPAND);
    sizer->Add(outer_, 1, wxEXPAND);
    root->SetSizer(sizer);

    CreateStatusBar(3);
    const int widths[] = {FromDIP(150), FromDIP(260), -1};
    GetStatusBar()->SetStatusWidths(3, widths);
    GetStatusBar()->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        wxRect files;
        if (GetStatusBar()->GetFieldRect(FIELD_TRANSFERS, files) && files.Contains(e.GetPosition()))
            tabs_->SetSelection(TAB_FILES);
        e.Skip();
    });

    // The connection bar.
    bar_->width->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { showGearshift(); });
    bar_->narrow->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { setNarrow(e.IsChecked()); });
    bar_->pause->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { setPaused(e.IsChecked()); });
    bar_->acceptSessions->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        link_.setAcceptSessions(e.IsChecked(), HostConnection::nowMs());
        saveBool(keys::ACCEPT_SESSIONS, e.IsChecked());
        applyPrefs(false);
        host_->flush();
    });

    // The station list.
    stationList_->onSelect([this](const std::string& call) { stationPicked(call); });
    stationList_->onActivate([this](const std::string& call) {
        tabs_->SetSelection(TAB_BROADCAST);
        broadcast_->address(call);
    });
    stationList_->onMenu([this](const std::string& call) { stationMenu(call); });

    // The Broadcast tab.
    broadcast_->onSend([this](const std::string& typed) { return sendTyped(typed); });
    broadcast_->onGear([this]() { showGearshift(); });
    broadcast_->onCancel([this](uint64_t id) {
        chat_.cancel(id);
        host_->flush();
        refreshMessages();
    });
    broadcast_->onResend([this](uint64_t id) {
        chat_.resend(id, broadcastMode(), HostConnection::nowMs());
        host_->flush();
        refreshMessages();
    });
    // The Session tab.
    sessionPage_->onSend([this](const std::string& typed) {
        if (session_.send(typed, HostConnection::nowMs()) == 0) return false;
        host_->flush();
        refreshSession();
        return true;
    });
    sessionPage_->onCancel([this]() { leaveSession(); });
    sessionPage_->onLeave([this]() { leaveSession(); });
    sessionPage_->onLeaveNow([this]() { leaveSessionNow(); });
    sessionPage_->onCallAgain([this]() { callStation(session_.peer()); });
    tabs_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
        updateTabLabels();
        placeTabToggle();
        e.Skip();
    });

    // Globus on and off, from its own toggle or the one on the tab strip.
    auto toggled = [this](wxCommandEvent& e) { setGlobus(e.IsChecked()); };
    globus_->toggle()->Bind(wxEVT_TOGGLEBUTTON, toggled);
    tabToggle_->Bind(wxEVT_TOGGLEBUTTON, toggled);
    middle_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        CallAfter([this]() { placeTabToggle(); });
    });
    GetMenuBar()->Check(ID_GLOBUS, globusOn_);
    stationList_->setNarrow(globusOn_);
    tabToggle_->Show(!globusOn_);
}

//-------------------------------------------------------------------------
// Settings
//-------------------------------------------------------------------------

void MainFrame::applyPrefs(bool reconnect)
{
    link_.setCallsign(prefs_.callsign);
    broadcast::BroadcastChat::Settings s;
    s.callsign = prefs_.callsign;
    s.locator = prefs_.locator;
    s.acceptsSessions = link_.settings().acceptSessions;
    s.software = software();
    chat_.setSettings(s, loadNewestItem(protocol::normalCallsign(prefs_.callsign)));
    session::SessionChat::Settings ss;
    ss.callsign = prefs_.callsign;
    ss.locator = prefs_.locator;
    ss.acceptsSessions = link_.settings().acceptSessions;
    ss.software = software();
    session_.setSettings(ss);
    stations_.setMyCallsign(prefs_.callsign);
    stations_.setHome(prefs_.locator);
    globus_->setHome(prefs_.locator);
    bar_->setCallsign(utf8(prefs_.callsign));
    SetTitle(prefs_.callsign.empty() ? wxString("Ragtime") : wxString::FromUTF8("Ragtime \xc2\xb7 ") + utf8(prefs_.callsign));

    if (reconnect)
    {
        if (prefs_.callsign.empty())
            host_->stop();
        else
            host_->start(prefs_.host, prefs_.kissPort, prefs_.commandPort);
    }
    showConnection();
    refreshStations();
}

void MainFrame::showSettings()
{
    SettingsDialog dialog(this, prefs_);
    if (dialog.ShowModal() != wxID_OK) return;
    StationPrefs next = dialog.prefs();
    bool reconnect = next.callsign != prefs_.callsign || next.host != prefs_.host || next.kissPort != prefs_.kissPort
                     || next.commandPort != prefs_.commandPort || host_->state() == HostConnection::State::Off;
    prefs_ = next;
    saveStationPrefs(prefs_);
    applyPrefs(reconnect);
}

//-------------------------------------------------------------------------
// The host
//-------------------------------------------------------------------------

void MainFrame::onHostEvents(const std::vector<Event>& events)
{
    uint64_t now = HostConnection::nowMs();
    for (const Event& e : events)
    {
        session_.onEvent(e, now);
        switch (e.type)
        {
            case Event::Type::Ptt:
                bar_->tx->setOn(e.on);
                break;
            case Event::Type::ModeUsed:
                modeUsed_ = e.text;
                break;
            case Event::Type::HeardFrom:
                stations_.heard(e.text, now);
                break;
            case Event::Type::ModesChanged:
            {
                std::vector<gearbox::HostMode> modes;
                for (const link::ModeInfo& m : link_.modes())
                {
                    gearbox::HostMode h;
                    h.name = m.name;
                    h.widthHz = m.bandwidthHz;
                    h.bitsPerSecond = gearbox::payloadBitsPerSecond(m.bytesPerCodeword, m.maxCodewords,
                                                                    m.secondsAtOne, m.secondsAtMax);
                    modes.push_back(h);
                }
                gearshift_.setHostModes(std::move(modes));
                showGear();
                break;
            }
            case Event::Type::GroupLost:
                note_ = "Heard " + std::to_string(e.count) + " burst" + (e.count == 1 ? "" : "s")
                        + " on D2G that didn't decode";
                break;
            case Event::Type::Refused:
                note_ = "data2g-host refused \"" + e.text + "\"";
                break;
            default:
                break;
        }
        for (const protocol::Record& r : chat_.onEvent(e, now)) stations_.onRecord(r, now);
    }
    onSessionEvents();
    showConnection();
    showStatus();
    refreshMessages();
    refreshSession();
    refreshStations();
}

void MainFrame::onClock()
{
    session_.tick(HostConnection::nowMs());
    onSessionEvents();
    host_->flush();
    refreshSession();
    showConnection();
    stationList_->refresh(HostConnection::nowMs());
    if (!stationList_->selected().empty()) stationPicked(stationList_->selected());
}

void MainFrame::showConnection()
{
    wxString state = utf8(host_ ? host_->describe() : std::string("Not connected"));
    wxString tip;
    if (prefs_.callsign.empty())
    {
        state = "Waiting for your callsign";
    }
    else if (host_ && host_->state() == HostConnection::State::Connected && !link_.groupOpen())
    {
        state = "Connected, opening D2G";
    }
    if (host_ && !host_->lastError().empty()) tip = utf8(host_->lastError());
    if (tip.empty()) tip = "Ragtime talks to a data2g-host you run; it never starts one. Settings > Station";
    bar_->setHost(utf8(prefs_.host + ":" + std::to_string(prefs_.commandPort)), state, tip);
}

void MainFrame::showStatus()
{
    SetStatusText(link_.busy() ? "Channel busy" : "Channel clear", FIELD_CHANNEL);
    std::string mode = !modeUsed_.empty() ? modeUsed_ : link_.groupMode();
    SetStatusText(mode.empty() ? wxString("Mode: the host's") : "Mode " + utf8(mode), FIELD_MODE);
    SetStatusText(note_.empty() ? wxString("No transfers") : utf8(note_), FIELD_TRANSFERS);
}

void MainFrame::refreshMessages()
{
    broadcast_->refresh();
    updateTabLabels();
}

void MainFrame::refreshStations()
{
    if (stations_.version() == stationsVersion_) return;
    stationsVersion_ = stations_.version();
    uint64_t now = HostConnection::nowMs();
    stationList_->refresh(now);
    std::vector<std::pair<std::string, bool>> dots;
    for (const broadcast::Station& s : stations_.stations())
    {
        if (!s.locator.empty()) dots.push_back({s.locator, true});
    }
    globus_->setStations(dots);
    stationPicked(stationList_->selected());
}

void MainFrame::stationPicked(const std::string& call)
{
    const broadcast::Station* s = call.empty() ? nullptr : stations_.find(call);
    if (s == nullptr)
    {
        globus_->setStation("", "");
        return;
    }
    uint64_t now = HostConnection::nowMs();
    std::string details = stations_.status(*s, now).word + " \xc2\xb7 heard " + broadcast::heardAgo(s->lastHeardMs, now);
    if (!s->software.empty()) details += "\n" + s->software;
    globus_->setStation(s->call, s->locator, true, details);
}

void MainFrame::stationMenu(const std::string& call)
{
    wxMenu menu;
    if (session_.active() && session_.peer() == call)
    {
        menu.Append(ID_STATION_LEAVE, session_.phase() == session::SessionChat::Phase::Calling
                                          ? "Stop calling " + utf8(call)
                                          : "Leave the session with " + utf8(call));
        menu.Enable(ID_STATION_LEAVE, session_.phase() != session::SessionChat::Phase::Leaving);
    }
    else
    {
        menu.Append(ID_STATION_CALL, "Call " + utf8(call) + "\tCtrl+K");
        menu.Enable(ID_STATION_CALL, canCall() || session_.active());
    }
    menu.Append(ID_STATION_MESSAGE, "Message @" + utf8(call) + "\tCtrl+M");
    menu.Append(ID_STATION_COPY, "Copy callsign");
    menu.Bind(wxEVT_MENU, [this, call](wxCommandEvent& e) {
        if (e.GetId() == ID_STATION_CALL) callStation(call);
        if (e.GetId() == ID_STATION_LEAVE) leaveSession();
        if (e.GetId() == ID_STATION_MESSAGE)
        {
            tabs_->SetSelection(TAB_BROADCAST);
            broadcast_->address(call);
        }
        else if (e.GetId() == ID_STATION_COPY && wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(utf8(call)));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&menu);
}

void MainFrame::updateTabLabels()
{
    // Messages heard while the Broadcast tab is out of sight count as unread.
    bool looking = tabs_->GetSelection() == TAB_BROADCAST && IsActive();
    size_t unread = 0;
    uint64_t newest = seenMessage_;
    for (const broadcast::Message& m : chat_.messages())
    {
        newest = std::max(newest, m.id);
        if (!m.ours && m.id > seenMessage_) unread++;
    }
    if (looking)
    {
        seenMessage_ = newest;
        unread = 0;
    }
    if (unread != unread_ || tabs_->GetPageText(TAB_BROADCAST) == wxEmptyString)
    {
        unread_ = unread;
        tabs_->SetPageText(TAB_BROADCAST, unread_ == 0 ? wxString("Broadcast")
                                                       : wxString::Format("Broadcast (%zu)", unread_));
        placeTabToggle();
    }

    // Session · W1AW, Session · calling W1AW, or greyed: Session · none.
    if (tabs_->GetSelection() == TAB_SESSION && IsActive()) sessionUnread_ = 0;
    wxString session = wxString::FromUTF8("Session \xc2\xb7 ");
    if (!session_.active())
        session += "none";
    else if (session_.phase() == session::SessionChat::Phase::Calling)
        session += "calling " + utf8(session_.peer());
    else
        session += utf8(session_.peer());
    if (sessionUnread_ > 0) session += wxString::Format(" (%zu)", sessionUnread_);
    if (tabs_->GetPageText(TAB_SESSION) != session)
    {
        tabs_->SetPageText(TAB_SESSION, session);
        placeTabToggle();
    }
}

//-------------------------------------------------------------------------
// Sessions
//-------------------------------------------------------------------------

bool MainFrame::canCall() const
{
    return !prefs_.callsign.empty() && link_.commandConnected() && host_ && host_->dataUp();
}

void MainFrame::callPrompt()
{
    std::string call = stationList_->selected();
    if (call.empty() || (session_.active() && call == session_.peer()))
    {
        wxTextEntryDialog ask(this, "The callsign of the station to call:", "Call a station");
        if (ask.ShowModal() != wxID_OK) return;
        call = std::string(ask.GetValue().Upper().utf8_str());
    }
    callStation(call);
}

void MainFrame::callStation(const std::string& call)
{
    std::string them = protocol::normalCallsign(call);
    if (them.empty()) return;
    uint64_t now = HostConnection::nowMs();

    // One session at a time: data2g-host runs one ARQ link.
    if (session_.active())
    {
        if (session_.peer() == them)
        {
            tabs_->SetSelection(TAB_SESSION);
            return;
        }
        wxMessageDialog ask(this,
                            "You're in a session with " + utf8(session_.peer()) + ". Leave it and call " + utf8(them)
                                + "?",
                            "One session at a time", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        ask.SetYesNoLabels("Leave and call", "Stay");
        if (ask.ShowModal() != wxID_YES) return;
        pendingCall_ = them;
        leaveSession();
        return;
    }
    if (!canCall())
    {
        note_ = host_ && !host_->dataUp() && link_.commandConnected()
                    ? "Can't call: data2g-host's session data port isn't connected"
                    : "Can't call: data2g-host isn't connected";
        showStatus();
        return;
    }

    // A station that's busy, or doesn't take sessions, is asked about first.
    if (const broadcast::Station* s = stations_.find(them))
    {
        broadcast::StatusView view = stations_.status(*s, now);
        std::string why;
        if (view.word == "QRL" || view.word.compare(0, 4, "QRL ") == 0)
            why = them + " is " + view.word + ": " + lowerFirst(view.how) + ".";
        else if (view.word == "No sessions")
            why = view.how + ".";
        if (!why.empty())
        {
            wxMessageDialog ask(this, utf8(why) + "\n\nCall anyway, or send it a message on D2G instead?",
                                "Call " + utf8(them), wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT | wxICON_QUESTION);
            ask.SetYesNoCancelLabels("Call anyway", "Message instead", "Cancel");
            int answer = ask.ShowModal();
            if (answer == wxID_NO)
            {
                tabs_->SetSelection(TAB_BROADCAST);
                broadcast_->address(them);
                return;
            }
            if (answer != wxID_YES) return;
        }
    }

    if (!session_.call(them, now))
    {
        note_ = "Couldn't call " + them;
        showStatus();
        return;
    }
    onSessionEvents();
    host_->flush();
    refreshSession();
}

void MainFrame::leaveSession()
{
    using Phase = session::SessionChat::Phase;
    if (!session_.active() || session_.phase() == Phase::Leaving)
    {
        if (!session_.active()) pendingCall_.clear();
        return;
    }
    // Files moving: ask, once.
    if (session_.phase() == Phase::Connected && session_.trafficBusy())
    {
        wxMessageDialog ask(this,
                            "A file is still moving with " + utf8(session_.peer())
                                + ". Leave anyway? It can carry on next time.",
                            "Leave the session", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        ask.SetYesNoLabels("Leave", "Stay");
        if (ask.ShowModal() != wxID_YES)
        {
            pendingCall_.clear();
            return;
        }
    }
    session_.leave(HostConnection::nowMs());
    host_->flush();
    refreshSession();
}

void MainFrame::leaveSessionNow()
{
    if (!session_.active()) return;
    session_.leaveNow(HostConnection::nowMs());
    host_->flush();
    refreshSession();
}

void MainFrame::onSessionData(const uint8_t* bytes, size_t length)
{
    uint64_t now = HostConnection::nowMs();
    // The far end's Hello tells the station list its locator; anything
    // heard in the session says it was heard.
    for (const protocol::Record& r : session_.dataBytes(bytes, length, now)) stations_.onRecord(r, now);
    onSessionEvents();
    refreshSession();
    refreshStations();
}

void MainFrame::onSessionEvents()
{
    using Type = session::SessionChat::Event::Type;
    uint64_t now = HostConnection::nowMs();
    for (const session::SessionChat::Event& e : session_.takeEvents())
    {
        switch (e.type)
        {
            case Type::Calling:
                // The tab opens at once, in the Calling state.
                stations_.setPartner(e.peer, false, now);
                sessionUnread_ = 0;
                tabs_->SetSelection(TAB_SESSION);
                break;
            case Type::Started:
                // Whoever called, the tab comes to the front, and the group
                // hears we're QRL (once the session goes idle).
                stations_.setPartner(e.peer, true, now);
                chat_.announce(protocol::Status::State::Qrl, e.peer, broadcastMode(), now);
                sessionUnread_ = 0;
                tabs_->SetSelection(TAB_SESSION);
                if (e.incoming && !IsActive()) RequestUserAttention(wxUSER_ATTENTION_INFO);
                break;
            case Type::Ended:
                stations_.setPartner("", false, now);
                chat_.announce(protocol::Status::State::Qrv, "", broadcastMode(), now);
                if (!pendingCall_.empty())
                {
                    std::string next = pendingCall_;
                    pendingCall_.clear();
                    CallAfter([this, next]() { callStation(next); });
                }
                break;
            case Type::Heard:
                if (!(tabs_->GetSelection() == TAB_SESSION && IsActive())) sessionUnread_++;
                break;
        }
    }
}

void MainFrame::refreshSession()
{
    sessionPage_->refresh(HostConnection::nowMs(), localOffsetSeconds());
    updateTabLabels();
    refreshStations();
}

//-------------------------------------------------------------------------
// Broadcasts
//-------------------------------------------------------------------------

std::string MainFrame::broadcastMode() const
{
    // The gear's mode, when data2g-host offers it; else the group's mode as
    // it stands, rather than a mode the host would refuse.
    std::optional<gearbox::Gear> gear = gearshift_.current();
    if (!gear) return std::string();
    for (const link::ModeInfo& m : link_.modes())
    {
        if (m.name == gear->mode) return gear->mode;
    }
    return std::string();
}

bool MainFrame::sendTyped(const std::string& typed)
{
    if (prefs_.callsign.empty())
    {
        showSettings();
        return false;
    }
    std::vector<uint64_t> ids = chat_.send(typed, broadcastMode(), HostConnection::nowMs());
    if (ids.empty()) return false;
    saveNewestItem(chat_.settings().callsign, chat_.newestItem());
    host_->flush();
    refreshMessages();
    return true;
}

void MainFrame::checkIn()
{
    if (chat_.checkIn(broadcastMode(), HostConnection::nowMs())) host_->flush();
}

void MainFrame::setPaused(bool paused)
{
    link_.setPaused(paused, HostConnection::nowMs());
    session_.setPaused(paused, HostConnection::nowMs());
    sessionPage_->setPaused(paused);
    bar_->setPaused(paused);
    GetMenuBar()->Check(ID_PAUSE, paused);
    broadcast_->setPaused(paused);
    saveBool(keys::PAUSED, paused);
    if (host_) host_->flush();
}

//-------------------------------------------------------------------------
// The gearshift
//-------------------------------------------------------------------------

void MainFrame::loadGearshift()
{
    wxArrayString sources;
    wxString errors;
    gearbox::SubmodeTable table = loadAllThresholds(sources, errors);
    if (!errors.empty()) wxLogWarning("Gearshift: %s", errors);
    gearshift_.setTable(std::move(table));
    ::loadGearshift(gearshift_);
    showGear();
}

void MainFrame::showGearshift()
{
    GearshiftDialog dialog(this, gearshift_, [this]() { gearChanged(); });
    dialog.ShowModal();
}

void MainFrame::gearChanged()
{
    // A width the host can take caps it: BW500 or BW2300.
    bool narrow = gearshift_.width() == gearbox::Width::Hz500;
    if (gearbox::hostSupportsWidth(gearshift_.width()) && narrow != link_.settings().narrow)
    {
        link_.setNarrow(narrow, HostConnection::nowMs());
        bar_->setNarrow(narrow);
        GetMenuBar()->Check(ID_NARROW, narrow);
        host_->flush();
    }
    saveGearshift(gearshift_);
    showGear();
}

void MainFrame::showGear()
{
    bar_->setWidthLabel(gearbox::widthLabel(gearshift_.width()));
    std::optional<gearbox::Gear> gear = gearshift_.current();
    if (!gear)
    {
        broadcast_->setGearLabel("Gear: the host's mode",
                                 "No gears yet: the gearshift says why. Until then broadcasts go in the group's "
                                 "mode as data2g-host has it.");
        return;
    }
    wxString tip = "Opens the gearshift";
    if (!link_.modes().empty() && broadcastMode().empty())
        tip += ". data2g-host doesn't offer this mode, so broadcasts go in the group's mode.";
    broadcast_->setGearLabel(utf8(gearbox::gearLabel(*gear)), tip);
}

void MainFrame::setNarrow(bool narrow)
{
    gearshift_.setWidth(narrow ? gearbox::Width::Hz500 : gearbox::Width::Hz2400);
    link_.setNarrow(narrow, HostConnection::nowMs());
    bar_->setNarrow(narrow);
    GetMenuBar()->Check(ID_NARROW, narrow);
    saveGearshift(gearshift_);
    showGear();
    if (host_) host_->flush();
}

//-------------------------------------------------------------------------
// Globus and the dividers
//-------------------------------------------------------------------------

void MainFrame::rememberDividers()
{
    if (outer_->IsSplit())
    {
        int left = outer_->GetSashPosition();
        if (globusOn_)
            listWidth_ = left;
        else
            tableWidth_ = left;
    }
    if (inner_->IsSplit()) globusWidth_ = std::max(MIN_PANE, inner_->GetClientSize().x - inner_->GetSashPosition());
}

void MainFrame::setGlobus(bool on)
{
    rememberDividers();
    globusOn_ = on;
    if (on && !inner_->IsSplit())
    {
        globus_->Show();
        inner_->SplitVertically(middle_, globus_, -globusWidth_);
    }
    else if (!on && inner_->IsSplit())
    {
        inner_->Unsplit(globus_);
    }
    globus_->showGlobe(true);
    tabToggle_->setOn(false);
    tabToggle_->Show(!on);
    stationList_->setNarrow(on);
    outer_->SetSashPosition(on ? listWidth_ : tableWidth_);
    GetMenuBar()->Check(ID_GLOBUS, on);
    saveBool(keys::GLOBUS, on);
    placeTabToggle();
}

void MainFrame::placeTabToggle()
{
    if (!tabToggle_->IsShown()) return;
    // The strip is the notebook's height above its pages.
    wxWindow* page = tabs_->GetCurrentPage();
    int strip = page ? page->GetPosition().y : 0;
    if (strip <= 0) strip = tabToggle_->GetBestSize().y + 8;
    wxSize size = tabToggle_->GetBestSize();
    int x = middle_->GetClientSize().x - size.x - FromDIP(6);
    int y = std::max(0, (strip - size.y) / 2);
    tabToggle_->SetSize(x, y, size.x, size.y);
    tabToggle_->Raise();
}
