#include "MainFrame.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/aboutdlg.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/config.h>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/notifmsg.h>
#include <wx/panel.h>
#include <wx/persist/toplevel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>

#include "AppIcon.h"
#include "BroadcastPage.h"
#include "ConnectionBar.h"
#include "DrawnIcon.h"
#include "FilesPage.h"
#include "GearshiftDialog.h"
#include "HostConnection.h"
#include "SessionPage.h"
#include "SettingsDialog.h"
#include "StationList.h"
#include "Theme.h"
#include "Thresholds.h"
#include "ToggleSwitch.h"
#include "TxLamp.h"
#include "globus/GlobusPanel.h"
#include "globus/GlobusToggle.h"
#include "ragtime/Version.h"
#include "ragtime/files/FileRows.h"
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
    ID_BROADCAST_FILE,
    ID_AUTO_RECEIVE,
    // A broadcast file's menu on the Files tab.
    ID_BROADCAST_FILE_TITLE,
    ID_BROADCAST_FILE_ACTION,
    ID_BROADCAST_FILE_ACTION_LAST = ID_BROADCAST_FILE_ACTION + 7,
    ID_SEND_FILE,
    ID_OPEN_RECEIVED,
    ID_STATION_SEND_FILE,
    // A file's right-click menu.
    ID_FILE_SAVE,
    ID_FILE_SAVE_AS,
    ID_FILE_DECLINE,
    ID_FILE_CANCEL,
    ID_FILE_UP,
    ID_FILE_DOWN,
    ID_FILE_ASK_REST,
    ID_FILE_KEEP_LONGER,
    ID_FILE_DELETE_PART,
    ID_FILE_ASK_AGAIN,
    ID_FILE_SEND_AGAIN,
};

constexpr size_t BROADCAST_FILE_ACTIONS = ID_BROADCAST_FILE_ACTION_LAST - ID_BROADCAST_FILE_ACTION + 1;

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
    FIELD_HOST,     // data2g-host's address and state
    FIELD_THEME,    // the light and dark button, at the right end where it can switch
};
const int STATUS_FIELDS = canSwitchTheme() ? 5 : 4;

constexpr int CLOCK_MS = 1000;
constexpr int MIN_PANE = 200;

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

const char* const DOT = " \xc2\xb7 ";

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

// "45 s", "3 min", "1 h 10 min".
std::string durationText(double seconds)
{
    long s = std::max(1L, (long)(seconds + 0.5));
    if (s < 60) return std::to_string(s) + " s";
    long minutes = (s + 30) / 60;
    if (minutes < 60) return std::to_string(minutes) + " min";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

// The local time so many seconds from now: "16:40".
wxString clockIn(double seconds)
{
    return (wxDateTime::Now() + wxTimeSpan::Seconds((wxLongLong)seconds)).Format("%H:%M");
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
    , seals_(link_, chat_)
    , broadcastFiles_(link_, chat_, seals_)
    , session_(link_)
    , fileTraffic_(sessionFiles_, history_)
    , clock_(this)
{
    session_.setTraffic(&fileTraffic_);
    sessionLog_.setFolder(sessionLogDir());
    session_.setLog(&sessionLog_);
    broadcastLog_.setPath(broadcastLogPath());
    chat_.setLog(&broadcastLog_, HostConnection::nowMs());
    SetIcons(appIcons());
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

    // Every broadcast item of ours is sealed once it has gone.
    chat_.onItems([this](const std::vector<protocol::Record>& items, uint64_t burst, const std::string& mode,
                         uint64_t now) { seals_.going(items, burst, mode, now); });
    setAutoReceive(loadBool(keys::AUTO_RECEIVE_BROADCAST, false));

    loadGearshift();
    setNarrow(gearshift_.narrowOnly());
    bar_->acceptSessions->SetValue(link_.settings().acceptSessions);
    setPaused(loadBool(keys::PAUSED, false));
    stationList_->setColumns(loadColumns());
    broadcast::Column sort = broadcast::Column::Heard;
    if (int saved = loadInt(keys::SORT_COLUMN, (int)sort); saved >= 0 && saved < broadcast::COLUMN_COUNT)
        sort = static_cast<broadcast::Column>(saved);
    stationList_->setSort(sort, loadBool(keys::SORT_ASCENDING, true));

    loadFilesLog();
    applyPrefs(true);
    showStatus();
    refreshFiles();

    wxPersistentRegisterAndRestore(this, "MainWindow");
    if (!IsMaximized() && GetPosition() == wxDefaultPosition) Centre();

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { onClock(); });
    clock_.Start(CLOCK_MS);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        // What is under way ends here; a part received is kept, and the
        // log keeps it for next time.
        sessionFiles_.stopAll("Ragtime was closed");
        fileTraffic_.update(HostConnection::nowMs());
        saveFilesLog(true);
        chat_.closeLog(HostConnection::nowMs());
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
    file->Append(ID_SEND_FILE, "&Send file...\tCtrl+O",
                 "Offers files to the station you're in a session with, or calls the station picked first");
    file->Append(ID_OPEN_RECEIVED, "Open &received folder", "Where files you receive go");
    file->AppendSeparator();
    file->Append(ID_BROADCAST_FILE, "&Broadcast a file...\tCtrl+Shift+B",
                 "Sends a file to everyone on D2G, with repairs for stations that miss pieces");
    file->AppendCheckItem(ID_AUTO_RECEIVE, "&Receive broadcast files automatically",
                          "Files broadcast on D2G are saved in the received folder without asking");
    file->AppendSeparator();
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
    station->Append(ID_STATION_SEND_FILE, "Send a &file...", "Offers files in the session, calling the station first if "
                                                             "you aren't in one with it");
    station->AppendSeparator();
    station->AppendCheckItem(ID_PAUSE, "&Pause sending\tCtrl+P");

    viewMenu_ = new wxMenu;
    viewMenu_->AppendCheckItem(ID_GLOBUS, "&Globus\tCtrl+Shift+G");
    auto* columns = new wxMenu;
    for (broadcast::Column c : broadcast::ALL_COLUMNS)
    {
        columns->AppendCheckItem(StationList::ID_COLUMN_FIRST + (int)c, broadcast::columnLabel(c));
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
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { broadcastFile(); }, ID_BROADCAST_FILE);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setAutoReceive(e.IsChecked()); }, ID_AUTO_RECEIVE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Enable(!prefs_.callsign.empty()); }, ID_BROADCAST_FILE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { showGearshift(); }, ID_GEARSHIFT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { showSettings(); }, ID_STATION_SETTINGS);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setNarrow(e.IsChecked()); }, ID_NARROW);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setGlobus(e.IsChecked()); }, ID_GLOBUS);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { setPaused(e.IsChecked()); }, ID_PAUSE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { checkIn(); }, ID_CHECK_IN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { callPrompt(); }, ID_CALL);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { leaveSession(); }, ID_LEAVE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { leaveSessionNow(); }, ID_LEAVE_NOW);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { sendFilePrompt(""); }, ID_SEND_FILE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { sendFilePrompt(""); }, ID_STATION_SEND_FILE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxFileName::Mkdir(wxString::FromUTF8(receivedFolder()), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        openWithDesktop(receivedFolder());
    }, ID_OPEN_RECEIVED);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(session_.phase() == session::SessionChat::Phase::Connected || canCall());
    }, ID_SEND_FILE, ID_STATION_SEND_FILE);
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
        broadcast::Column c = static_cast<broadcast::Column>(index);
        const auto& shown = stationList_->columns();
        e.Check(std::find(shown.begin(), shown.end(), c) != shown.end());
        e.Enable(c != broadcast::Column::Call);
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
    sessionPage_ = new SessionPage(tabs_, session_, fileTraffic_);
    tabs_->AddPage(sessionPage_, wxString::FromUTF8("Session \xc2\xb7 none"));
    filesPage_ = new FilesPage(tabs_, history_);
    tabs_->AddPage(filesPage_, "Files");
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

    // Our own tooltips, not wx's wxSTB_SHOW_TIPS: with it on, wx asserts
    // on any tooltip set by hand.
    CreateStatusBar(STATUS_FIELDS, wxSTB_DEFAULT_STYLE & ~wxSTB_SHOW_TIPS);
    if (canSwitchTheme())
    {
        themeButton_ = new wxBitmapButton(GetStatusBar(), wxID_ANY, wxBitmapBundle(), wxDefaultPosition,
                                          wxDefaultSize, wxBU_EXACTFIT | wxBORDER_NONE);
        showThemeButton();
        themeButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            ThemeChoice choice = themeIsDark() ? ThemeChoice::Light : ThemeChoice::Dark;
            saveInt(keys::THEME, (int)choice);
            applyTheme(choice);
        });
        // From the button, or the desktop switching under us.
        Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
            e.Skip();
            showThemeButton();
        });
    }
    sizeStatusFields();
    // The theme button sits in its field, which moves as the window resizes
    // (and maximizing hides the size grip beside it).
    GetStatusBar()->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        CallAfter([this]() { sizeStatusFields(); });
    });
    GetStatusBar()->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        wxRect files;
        if (GetStatusBar()->GetFieldRect(FIELD_TRANSFERS, files) && files.Contains(e.GetPosition()))
            tabs_->SetSelection(TAB_FILES);
        e.Skip();
    });
    // On hover the host's field explains itself (or gives the last error),
    // and a field cut short shows its whole text, as wxSTB_SHOW_TIPS would.
    GetStatusBar()->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        wxStatusBar* bar = GetStatusBar();
        wxString tip;
        for (int i = 0; i < FIELD_THEME; i++)
        {
            wxRect field;
            if (!bar->GetFieldRect(i, field) || !field.Contains(e.GetPosition())) continue;
            wxString text = bar->GetStatusText(i);
            if (i == FIELD_HOST)
                tip = hostTip_;
            else if (i == FIELD_CHANNEL)
                tip = channelTip_;
            else if (bar->GetTextExtent(text).x > field.width - FromDIP(8))
                tip = text;
        }
        if (bar->GetToolTipText() != tip) bar->SetToolTip(tip);
        e.Skip();
    });

    // The connection bar.
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
    broadcast_->nameModes([this](const std::string& mode) { return gearshift_.shortName(mode); });
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
    broadcast_->onBroadcastFile([this]() { broadcastFile(); });
    broadcast_->fileCards([this](uint64_t id, FileCard& card) { return fileCard(id, card); });
    broadcast_->onFileAction([this](uint64_t id, const std::string& action) { broadcastFileAction(id, action); });
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
    sessionPage_->onResend([this](uint64_t id) {
        if (session_.resend(id, HostConnection::nowMs()) == 0) return;
        host_->flush();
        refreshSession();
    });
    sessionPage_->onSendFile([this]() { sendFilePrompt(session_.peer()); });
    sessionPage_->onDropFiles([this](const std::vector<std::string>& paths) {
        if (session_.phase() == session::SessionChat::Phase::Connected)
            queueFiles(session_.peer(), paths);
        else
            wxMessageBox("Files go in a session: call a station first, then drop them here.", "Send a file",
                         wxOK | wxICON_INFORMATION, this);
    });
    sessionPage_->onFileAction([this](const std::string& action, uint64_t id) { fileAction(action, id); });
    // The Files tab: each row's menu comes from what moves it.
    filesPage_->onRowMenu([this](const files::FileRow& row, wxMenu& menu) {
        if (row.via == files::Via::Session) sessionFileMenu(row, menu);
        else broadcastFileMenu(row, menu);
    });
    filesPage_->onFinishedMenu([this](const files::FinishedFile& file, wxMenu& menu) { finishedFileMenu(file, menu); });
    filesPage_->onOpenFolder([this]() {
        wxFileName::Mkdir(wxString::FromUTF8(receivedFolder()), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        openWithDesktop(receivedFolder());
    });
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
    seals_.setChain(prefs_.callsign, loadSealChain(protocol::normalCallsign(prefs_.callsign)));
    savedSeal_ = seals_.chain().number;
    seals_.setSealText(prefs_.sealChat);
    session::SessionChat::Settings ss;
    ss.callsign = prefs_.callsign;
    ss.locator = prefs_.locator;
    ss.acceptsSessions = link_.settings().acceptSessions;
    ss.software = software();
    session_.setSettings(ss);
    stations_.setMyCallsign(prefs_.callsign);
    stations_.setHome(prefs_.locator);
    applyFilePrefs();
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
                stations_.heard(e.text, now, e.snr);
                updatePathSnr(now);
                break;
            case Event::Type::SessionSnr:
                stations_.heard(e.peer, now, e.snr);
                updatePathSnr(now);
                break;
            case Event::Type::Missed:
            {
                // Any group's burst, maybe not D2G's: only a hint.
                std::string snr = e.snr ? " at " + gearbox::formatDb(*e.snr) : std::string();
                note_ = "Heard a " + gearshift_.shortName(e.text) + " burst" + snr
                        + " that didn't decode; it may not be D2G's";
                break;
            }
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
        seals_.onEvent(e, now);
        broadcastFiles_.onEvent(e, now);
        for (const protocol::Record& r : chat_.onEvent(e, now))
        {
            stations_.onRecord(r, now);
            seals_.onRecord(r, now);
            broadcastFiles_.onRecord(r, now);
        }
    }
    onBroadcastFiles();
    onSessionEvents();
    onFileEvents();
    showConnection();
    showStatus();
    refreshMessages();
    refreshSession();
    refreshStations();
}

void MainFrame::onClock()
{
    uint64_t now = HostConnection::nowMs();
    updatePathSnr(now);
    seals_.setMode(broadcastMode());
    seals_.tick(now);
    broadcastFiles_.tick(now);
    onBroadcastFiles();
    session_.tick(now);
    fileTraffic_.tick(now);
    onSessionEvents();
    onFileEvents();
    host_->flush();
    refreshFiles();
    saveFilesLog(false);
    refreshSession();
    showConnection();
    showStatus();
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
    hostTip_ = tip;
    wxString text = "data2g-host " + utf8(prefs_.host + ":" + std::to_string(prefs_.commandPort)) + utf8(DOT) + state;
    if (text == hostText_) return;
    hostText_ = text;
    SetStatusText(text, FIELD_HOST);
    sizeStatusFields();
}

void MainFrame::sizeStatusFields()
{
    // The transfers line takes what the other fields leave, and the host's
    // field is as wide as its text. The theme button's field comes last:
    // on GTK the size grip, shown unless the window is maximized, sits to
    // its right, and wx cuts the last field's text short to clear it, so
    // the host's text keeps clear of the last field.
    wxStatusBar* bar = GetStatusBar();
    if (bar == nullptr) return;
    wxString shown = hostText_.empty() ? utf8(std::string("data2g-host 127.0.0.1:8300") + DOT + "Not connected")
                                       : hostText_;
    int host = bar->GetTextExtent(shown).x + 2 * bar->GetBorderX() + FromDIP(12);
    if (themeButton_ == nullptr && !IsMaximized()) host += bar->GetClientSize().y;
    const int theme = themeButton_ != nullptr ? bar->GetClientSize().y + 2 * bar->GetBorderX() : 0;
    const int widths[5] = {FromDIP(270), FromDIP(260), -1, host, theme};
    bar->SetStatusWidths(STATUS_FIELDS, widths);

    wxRect field;
    if (themeButton_ != nullptr && bar->GetFieldRect(FIELD_THEME, field))
    {
        // Not while the bar is still too small to hold it: GTK complains.
        const int side = std::min(field.width, field.height);
        if (side >= FromDIP(12))
            themeButton_->SetSize(field.x + (field.width - side) / 2, field.y + (field.height - side) / 2, side,
                                  side);
    }
}

void MainFrame::showThemeButton()
{
    if (themeButton_ == nullptr) return;
    const bool dark = themeIsDark();
    themeButton_->SetBitmap(themeIcon(dark, wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    themeButton_->SetToolTip(dark ? "Light mode" : "Dark mode");
}

void MainFrame::showStatus()
{
    // Channel clear or busy, or what of ours is going out, and on hover
    // all of it.
    wxString channel = utf8(link::channelLine(link_));
    if (GetStatusBar()->GetStatusText(FIELD_CHANNEL) != channel) SetStatusText(channel, FIELD_CHANNEL);
    channelTip_ = utf8(link::channelTip(link_));
    std::string mode = !modeUsed_.empty() ? modeUsed_ : link_.groupMode();
    SetStatusText(mode.empty() ? wxString("Mode: the host's") : "Mode " + utf8(gearshift_.shortName(mode)),
                  FIELD_MODE);
    SetStatusText(utf8(transfersLine()), FIELD_TRANSFERS);
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
    menu.Append(ID_STATION_SEND_FILE, "Send a file to " + utf8(call) + "...");
    menu.Enable(ID_STATION_SEND_FILE, canCall() || (session_.phase() == session::SessionChat::Phase::Connected));
    menu.Append(ID_STATION_COPY, "Copy callsign");
    menu.Bind(wxEVT_MENU, [this, call](wxCommandEvent& e) {
        if (e.GetId() == ID_STATION_CALL) callStation(call);
        if (e.GetId() == ID_STATION_SEND_FILE) CallAfter([this, call]() { sendFilePrompt(call); });
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
    // A burst of the partner's decoded: ask data2g-host how strong it was.
    link_.sessionHeard(now);
    onSessionEvents();
    onFileEvents();
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
                // A request saved for this station ("send it again") goes now.
                if (auto ask = askOnConnect_.find(e.peer); ask != askOnConnect_.end())
                {
                    session_.send(ask->second, now);
                    askOnConnect_.erase(ask);
                }
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
// Files
//-------------------------------------------------------------------------

void MainFrame::applyFilePrefs()
{
    // Auto-accept saves straight into the received folder.
    sessionFiles_.setAutoAccept(receivedFolder(), loadAutoAccept());
}

void MainFrame::loadFilesLog()
{
    wxString path = utf8(filesLogPath());
    if (!wxFileName::FileExists(path)) return;
    wxFFile file(path, "rb");
    wxString text;
    if (!file.IsOpened() || !file.ReadAll(&text, wxConvUTF8)) return;
    std::vector<files::KeptPart> kept;
    files::FileHistory history;
    if (!files::loadFilesText(std::string(text.utf8_str()), history, kept, &logOther_))
    {
        wxLogWarning("The files log %s isn't Ragtime's; it is left alone and a new one kept beside it.", path);
        return;
    }
    for (const files::FinishedFile& f : history.entries()) history_.add(f);
    sessionFiles_.restoreKept(kept, HostConnection::nowMs());
}

void MainFrame::saveFilesLog(bool force)
{
    // Saved when the log or the kept parts change, not as bytes move.
    uint64_t state = history_.version() * 1000003u;
    for (const files::KeptPart& k : sessionFiles_.keptParts())
        state = state * 31 + k.bytes + k.keepUntilMs * 7 + std::hash<std::string>()(k.part);
    if (!force && state == savedLog_) return;
    savedLog_ = state;
    wxFileName path(utf8(filesLogPath()));
    // A log that can't be saved says so in the status bar, not in a
    // dialog every time a transfer changes.
    wxLogNull quiet;
    path.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxTempFile out(path.GetFullPath());
    std::string text = files::saveFilesText(history_, sessionFiles_.keptParts(), logOther_);
    if (!out.IsOpened() || !out.Write(text.data(), text.size()) || !out.Commit())
        note_ = "Couldn't save the files log, " + std::string(path.GetFullPath().utf8_str());
}

void MainFrame::refreshFiles()
{
    uint64_t now = HostConnection::nowMs();
    std::vector<files::FileRow> rows = fileTraffic_.rows(now);
    for (files::FileRow& r : broadcastFiles_.rows(now)) rows.push_back(std::move(r));
    filesPage_->setRows(std::move(rows), now, localOffsetSeconds());
    showStatus();
}

std::string MainFrame::transfersLine() const
{
    uint64_t now = HostConnection::nowMs();
    const files::FileRow* newest = nullptr;
    std::vector<files::FileRow> rows = fileTraffic_.rows(now);
    for (files::FileRow& r : broadcastFiles_.rows(now)) rows.push_back(std::move(r));
    for (const files::FileRow& r : rows)
    {
        if (r.look == files::FileRow::Look::Kept) continue;
        if (newest == nullptr || r.atMs > newest->atMs) newest = &r;
    }
    if (newest == nullptr) return note_.empty() ? std::string("No transfers") : note_;
    std::string line = newest->name + (newest->outgoing ? " to " : " from ") + newest->peer;
    if (newest->look == files::FileRow::Look::Moving || newest->look == files::FileRow::Look::Stalled)
        line += DOT + files::progressText(newest->done, newest->size);
    return line + DOT + newest->state;
}

void MainFrame::onFileEvents()
{
    using Type = files::FileTraffic::Event::Type;
    for (const files::FileTraffic::Event& e : fileTraffic_.takeEvents())
    {
        const files::FileTransfer* t = sessionFiles_.transfer(e.id);
        if (e.type == Type::Offered && t != nullptr)
        {
            // An offer is a card in the session; the tab and, in the
            // background, the desktop draw attention to it.
            std::string what = t->peer + (e.autoAccepted ? " is sending " : " offers ") + t->name + ", "
                               + files::sizeText(t->size);
            note_ = what;
            if (!(tabs_->GetSelection() == TAB_SESSION && IsActive())) sessionUnread_++;
            if (!IsActive())
            {
                wxNotificationMessage notice("Ragtime", utf8(what), this);
                notice.Show();
                RequestUserAttention(wxUSER_ATTENTION_INFO);
            }
        }
        else if (e.type == Type::Finished)
        {
            if (const files::FinishedFile* f = history_.find(e.historyId))
            {
                note_ = f->name + (f->outgoing ? " to " : " from ") + f->peer + ": " + files::resultLabel(*f);
                if (f->via == files::Via::Session)
                {
                    // One line in the station's session log, for the
                    // next session with it to show.
                    session::Message line;
                    line.kind = session::Message::Kind::File;
                    line.ours = f->outgoing;
                    line.from = f->outgoing ? session_.settings().callsign : f->peer;
                    line.atMs = f->atMs;
                    line.text = f->name + ", " + files::sizeText(f->size) + ": " + files::resultLabel(*f);
                    sessionLog_.file(f->peer, line);
                }
            }
        }
    }
    updateTabLabels();
}

void MainFrame::sendFilePrompt(const std::string& call)
{
    using Phase = session::SessionChat::Phase;
    std::string them = protocol::normalCallsign(call);
    if (them.empty()) them = session_.active() ? session_.peer() : stationList_->selected();
    if (them.empty())
    {
        wxTextEntryDialog ask(this, "The callsign of the station to send files to:", "Send a file");
        if (ask.ShowModal() != wxID_OK) return;
        them = protocol::normalCallsign(std::string(ask.GetValue().Upper().utf8_str()));
        if (them.empty()) return;
    }
    bool inSession = session_.peer() == them && (session_.phase() == Phase::Connected
                                                 || session_.phase() == Phase::Calling);
    if (!inSession)
    {
        // There is no hidden session for a file: the operator calls.
        wxMessageDialog ask(this,
                            "Files go in a session. Call " + utf8(them)
                                + " and send them once you're connected?",
                            "Send a file to " + utf8(them), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
        ask.SetYesNoLabels("Pick files and call", "Cancel");
        if (ask.ShowModal() != wxID_YES) return;
    }
    wxFileDialog pick(this, "Send to " + utf8(them), wxEmptyString, wxEmptyString, "All files|*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (pick.ShowModal() != wxID_OK) return;
    wxArrayString picked;
    pick.GetPaths(picked);
    std::vector<std::string> paths;
    for (const wxString& p : picked) paths.push_back(std::string(p.utf8_str()));
    queueFiles(them, paths);
    if (!inSession) callStation(them);
}

void MainFrame::queueFiles(const std::string& call, const std::vector<std::string>& paths)
{
    uint64_t now = HostConnection::nowMs();
    std::string problems;
    for (const std::string& path : paths)
    {
        std::string error;
        if (sessionFiles_.offer(call, path, now, error) == 0) problems += error + "\n";
    }
    fileTraffic_.update(now);
    host_->flush();
    refreshSession();
    refreshFiles();
    if (!problems.empty()) wxMessageBox(utf8(problems), "Send a file", wxOK | wxICON_WARNING, this);
}

void MainFrame::saveOffer(uint64_t id, bool ask)
{
    const files::FileTransfer* t = sessionFiles_.transfer(id);
    if (t == nullptr || t->state != files::FileTransfer::State::Asking) return;
    std::string folder = receivedFolder();
    std::string path;
    if (t->kept > 0 && !t->keptPath.empty())
    {
        // Carries on in the part kept from before.
        path = t->keptPath;
        folder = std::string(wxFileName(utf8(path)).GetPath().utf8_str());
    }
    wxFileName::Mkdir(utf8(folder), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (path.empty()) path = files::utf8FromPath(files::freeSavePath(files::pathFromUtf8(folder), t->name));
    if (ask)
    {
        std::string name = t->name;
        wxFileDialog save(this, "Save " + utf8(name) + " from " + utf8(t->peer), utf8(folder),
                          wxFileName(utf8(path.empty() ? name : path)).GetFullName(), "All files|*",
                          wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (save.ShowModal() != wxID_OK) return;
        path = std::string(save.GetPath().utf8_str());
        // The offer may have gone while the dialog was up.
        t = sessionFiles_.transfer(id);
        if (t == nullptr || t->state != files::FileTransfer::State::Asking) return;
    }
    if (path.empty()) return;
    std::string error;
    if (!sessionFiles_.accept(id, path, error)) wxMessageBox(utf8(error), "Save the file", wxOK | wxICON_WARNING, this);
}

void MainFrame::fileAction(const std::string& action, uint64_t id)
{
    const files::FileTransfer* t = sessionFiles_.transfer(id);
    if (t == nullptr) return;
    if (action == "save" || action == "saveas")
        saveOffer(id, action == "saveas");
    else if (action == "decline")
        sessionFiles_.decline(id);
    else if (action == "cancel")
        sessionFiles_.cancel(id);
    else if (action == "up" || action == "down")
        sessionFiles_.reorder(id, action == "up" ? -1 : 1);
    else if (action == "open")
        openWithDesktop(t->path);
    else if (action == "folder")
        openWithDesktop(std::string(wxFileName(utf8(t->path)).GetPath().utf8_str()));
    fileTraffic_.update(HostConnection::nowMs());
    host_->flush();
    refreshSession();
    refreshFiles();
}

void MainFrame::askAgain(const std::string& call, const std::string& name, uint64_t kept, uint64_t size)
{
    std::string text = "Please send " + name + " again";
    if (kept > 0 && kept < size)
        text += ": I have " + files::progressText(kept, size) + " bytes kept, so only the rest needs to come.";
    else
        text += ".";
    if (session_.phase() == session::SessionChat::Phase::Connected && session_.peer() == call)
    {
        session_.send(text, HostConnection::nowMs());
        host_->flush();
        tabs_->SetSelection(TAB_SESSION);
        refreshSession();
        return;
    }
    wxMessageDialog ask(this, "Call " + utf8(call) + " first? The request goes into the session once you're connected:\n\n\"" + utf8(text) + "\"",
                        "Ask " + utf8(call), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
    ask.SetYesNoLabels("Call", "Cancel");
    if (ask.ShowModal() != wxID_YES) return;
    askOnConnect_[call] = text;
    callStation(call);
}

void MainFrame::sessionFileMenu(const files::FileRow& row, wxMenu& menu)
{
    using Look = files::FileRow::Look;
    uint64_t id = row.id;
    if (row.look == Look::Kept)
    {
        // A part kept from before.
        std::string peer = row.peer, name = row.name;
        uint64_t kept = row.done, size = row.size;
        menu.Append(ID_FILE_ASK_REST, "&Ask " + utf8(peer) + " to send the rest");
        menu.Append(ID_FILE_KEEP_LONGER, "&Keep 7 days longer");
        menu.AppendSeparator();
        menu.Append(ID_FILE_DELETE_PART, "&Delete part");
        menu.Bind(wxEVT_MENU, [=](wxCommandEvent&) { CallAfter([=]() { askAgain(peer, name, kept, size); }); },
                  ID_FILE_ASK_REST);
        menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent&) {
            sessionFiles_.keepLonger(id, files::SessionFiles::KEPT_MS, HostConnection::nowMs());
            refreshFiles();
        }, ID_FILE_KEEP_LONGER);
        menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent&) {
            sessionFiles_.deleteKept(id);
            refreshFiles();
        }, ID_FILE_DELETE_PART);
        return;
    }
    const files::FileTransfer* t = sessionFiles_.transfer(id);
    if (t == nullptr) return;
    auto act = [this, id](const char* action) {
        std::string what = action;
        return [this, id, what](wxCommandEvent&) { CallAfter([this, id, what]() { fileAction(what, id); }); };
    };
    switch (t->state)
    {
        case files::FileTransfer::State::Asking:
            menu.Append(ID_FILE_SAVE, t->kept > 0 ? "&Save (carry on from the part kept)" : "&Save");
            menu.Append(ID_FILE_SAVE_AS, "Save &as...");
            menu.Append(ID_FILE_DECLINE, "&Decline");
            menu.Bind(wxEVT_MENU, act("save"), ID_FILE_SAVE);
            menu.Bind(wxEVT_MENU, act("saveas"), ID_FILE_SAVE_AS);
            menu.Bind(wxEVT_MENU, act("decline"), ID_FILE_DECLINE);
            break;
        case files::FileTransfer::State::Waiting:
            menu.Append(ID_FILE_UP, "Move &up the queue");
            menu.Append(ID_FILE_DOWN, "Move &down the queue");
            menu.AppendSeparator();
            menu.Append(ID_FILE_CANCEL, "&Remove from the queue");
            menu.Bind(wxEVT_MENU, act("up"), ID_FILE_UP);
            menu.Bind(wxEVT_MENU, act("down"), ID_FILE_DOWN);
            menu.Bind(wxEVT_MENU, act("cancel"), ID_FILE_CANCEL);
            break;
        default:
            menu.Append(ID_FILE_CANCEL, "&Cancel");
            menu.Bind(wxEVT_MENU, act("cancel"), ID_FILE_CANCEL);
            break;
    }
}

void MainFrame::finishedFileMenu(const files::FinishedFile& file, wxMenu& menu)
{
    using Result = files::FinishedFile::Result;
    // A broadcast file's finished row needs nothing past what the tab
    // gives every row (Open, Open folder, Remove from list).
    if (file.via != files::Via::Session) return;
    std::string peer = file.peer, name = file.name, path = file.path;
    if (!file.outgoing && file.result != Result::Saved && file.result != Result::Declined
        && file.result != Result::Cancelled)
    {
        uint64_t kept = 0, size = file.size;
        for (const files::KeptPart& k : sessionFiles_.keptParts())
        {
            if (k.peer == peer && k.size == size && k.name == name) kept = k.bytes;
        }
        menu.Append(ID_FILE_ASK_AGAIN, "&Ask " + utf8(peer) + " to send it again");
        menu.Bind(wxEVT_MENU, [=](wxCommandEvent&) { CallAfter([=]() { askAgain(peer, name, kept, size); }); },
                  ID_FILE_ASK_AGAIN);
    }
    if (file.outgoing && file.result != Result::Sent && !path.empty())
    {
        menu.Append(ID_FILE_SEND_AGAIN, "&Send it to " + utf8(peer) + " again");
        menu.Enable(ID_FILE_SEND_AGAIN, wxFileName::FileExists(utf8(path)));
        menu.Bind(wxEVT_MENU, [=](wxCommandEvent&) {
            CallAfter([=]() {
                bool inSession = session_.peer() == peer && session_.phase() == session::SessionChat::Phase::Connected;
                queueFiles(peer, {path});
                if (!inSession) callStation(peer);
            });
        }, ID_FILE_SEND_AGAIN);
    }
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
// Broadcast files
//-------------------------------------------------------------------------

void MainFrame::broadcastFile()
{
    if (prefs_.callsign.empty())
    {
        showSettings();
        return;
    }
    wxFileDialog pick(this, "Broadcast a file to D2G", "", "", "All files|*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() != wxID_OK) return;
    wxString path = pick.GetPath();
    wxString name = wxFileName(path).GetFullName();
    wxULongLong size = wxFileName::GetSize(path);
    if (size == wxInvalidSize || size == 0)
    {
        wxMessageBox(name + " is empty or can't be read.", "Broadcast a file", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (size.GetValue() > broadcast::BroadcastFiles::MAX_FILE_BYTES)
    {
        wxMessageBox(wxString::Format("%s is %s bytes. A file over HF can be 300 KB at most.", name,
                                      utf8(files::groupedNumber(size.GetValue()))),
                     "Broadcast a file", wxOK | wxICON_INFORMATION, this);
        return;
    }

    std::string mode = broadcastMode();
    std::string nameUtf8(name.utf8_str());
    broadcast::FileEstimate estimate = broadcastFiles_.estimate(size.GetValue(), nameUtf8, mode);
    std::optional<gearbox::Gear> gear = gearshift_.current();
    wxString where = gear && !mode.empty() ? utf8(gearbox::gearLabel(*gear))
                                           : "the group's mode" + (link_.groupMode().empty()
                                                                       ? wxString()
                                                                       : " (" + utf8(gearshift_.shortName(link_.groupMode())) + ")");
    // The faster gears the host offers, for a file that would take long.
    wxString faster;
    for (const gearbox::Gear& g : gearshift_.ladder())
    {
        if (gear && g.number <= gear->number) continue;
        bool offered = std::any_of(link_.modes().begin(), link_.modes().end(),
                                   [&](const link::ModeInfo& m) { return m.name == g.mode; });
        if (!offered) continue;
        broadcast::FileEstimate at = broadcastFiles_.estimate(size.GetValue(), nameUtf8, g.mode);
        if (!faster.empty()) faster += ", ";
        faster += wxString::Format("gear %d: %s", g.number, utf8(durationText(at.airSeconds)));
    }

    if (estimate.airSeconds > broadcast::BroadcastFiles::REFUSE_AIR_SECONDS)
    {
        wxString text = wxString::Format("In %s, %s would take about %s on the air. A broadcast file may take "
                                         "two hours at most.",
                                         where, name, utf8(durationText(estimate.airSeconds)));
        if (!faster.empty()) text += "\n\nFaster: " + faster + ".";
        wxMessageBox(text, "Broadcast a file", wxOK | wxICON_INFORMATION, this);
        return;
    }
    std::string air = durationText(estimate.airSeconds), wall = durationText(estimate.wallSeconds);
    wxString text = wxString::Format(
        "%s, %s bytes, %d pieces in %s: about %s on the air%s, then repairs until %s at the latest. Stations "
        "will not confirm receipt.",
        name, utf8(files::groupedNumber(size.GetValue())), estimate.pieces, where, utf8(air),
        wall == air ? wxString() : " (" + utf8(wall) + " with the gaps between bursts)",
        clockIn(estimate.wallSeconds + (double)estimate.repairsMs / 1000.0));
    bool long_ = estimate.airSeconds > broadcast::BroadcastFiles::WARN_AIR_SECONDS;
    if (long_)
    {
        text += "\n\nThat holds the D2G group for a long time.";
        if (!faster.empty()) text += " Faster: " + faster + ".";
    }
    wxMessageDialog confirm(this, text + "\n\nBroadcast it to D2G?", "Broadcast a file",
                            wxYES_NO | (long_ ? wxNO_DEFAULT | wxICON_WARNING : wxICON_QUESTION));
    confirm.SetYesNoLabels("Broadcast", "Cancel");
    if (confirm.ShowModal() != wxID_YES) return;

    std::string error;
    uint64_t id = broadcastFiles_.send(std::string(path.utf8_str()), mode, HostConnection::nowMs(), error);
    if (id == 0)
    {
        wxMessageBox(utf8(error) + ".", "Broadcast a file", wxOK | wxICON_INFORMATION, this);
        return;
    }
    saveNewestItem(chat_.settings().callsign, chat_.newestItem());
    onBroadcastFiles();
    host_->flush();
    tabs_->SetSelection(TAB_BROADCAST);
}

bool MainFrame::fileCard(uint64_t id, FileCard& card)
{
    using State = broadcast::BroadcastFile::State;
    uint64_t now = HostConnection::nowMs();
    broadcast::BroadcastFile f;
    if (!broadcastFiles_.find(id, now, f)) return false;
    card.what = f.name + " \xc2\xb7 " + files::sizeText(f.size);
    std::string pieces = std::to_string(f.held) + " of " + std::to_string(f.pieces) + " pieces";
    switch (f.state)
    {
        case State::Sending:
            card.state = "Sending, " + pieces;
            card.actions = {{"stop", "Stop"}};
            card.menu = {{"stop", "Stop sending (repairs stay open)"}};
            break;
        case State::Repairing:
            if (!f.sealed)
                card.state = "Sealing";
            else
                card.state = "Repairs open until " + files::expiryText(f.repairsUntilMs, now, localOffsetSeconds())
                             + (f.askers == 0 ? std::string()
                                              : ", " + std::to_string(f.askers) + (f.askers == 1 ? " station" : " stations")
                                                    + " asked");
            // Stopped part way: what went is sealed, and the rest goes to
            // a station that asks for it while repairs are open.
            if (f.stopped) card.state = "Stopped at " + pieces + ". " + card.state;
            if (f.sealed)
            {
                card.actions = {{"keep", "Keep open 30 min longer"}, {"close", "Close repairs now"}};
                card.title = "Repairs open until " + files::expiryText(f.repairsUntilMs, now, localOffsetSeconds());
                card.menu = {{"keep", "Keep open 30 min longer"},
                             {"keep2h", "Keep open 2 hours longer"},
                             {"keepuntil", "Keep open until..."},
                             {"close", "Close repairs now"}};
            }
            break;
        case State::Sent:
            card.state = "Sent";
            card.good = true;
            break;
        case State::Cancelled:
            card.state = "Stopped";
            break;
        // Theirs. Rejecting throws away what came, so it is only on the
        // right-click menu, and asks first.
        case State::Heard:
            card.state = f.held == f.pieces ? std::string("All here") : "Heard, " + pieces;
            card.actions = {{"receive", "Receive..."}};
            card.menu = {{"receive", "Receive..."}, {"reject", "Reject file..."}};
            break;
        case State::Receiving:
            card.state = f.asking ? "Asking for " + std::to_string(f.pieces - f.held) + " pieces"
                                  : "Receiving, " + pieces;
            card.actions = {{"stopasking", "Stop"}};
            card.menu = {{"stopasking", "Stop (keep what came)"}, {"reject", "Reject file..."}};
            break;
        case State::Incomplete:
            card.state = std::string(f.stopped ? "Stopped, " : "Incomplete, ") + pieces + " kept";
            card.actions = {{"askagain", "Ask again"}};
            card.menu = {{"askagain", "Ask again for the rest"}, {"reject", "Reject file..."}};
            break;
        case State::Saved:
            card.state = f.autoReceived ? "Saved without asking" : "Saved";
            card.good = true;
            card.actions = {{"open", "Open"}, {"folder", "Open folder"}};
            break;
        case State::Ignored:
            card.state = "Rejected";
            break;
        case State::Failed:
            card.state = "Failed" + (f.error.empty() ? std::string() : ": " + f.error);
            card.bad = true;
            break;
        case State::Expired:
            card.state = "Not received";
            break;
    }
    return true;
}

void MainFrame::receiveFile(uint64_t id)
{
    broadcast::BroadcastFile f;
    if (!broadcastFiles_.find(id, HostConnection::nowMs(), f)) return;
    wxString folder = utf8(receivedFolder());
    wxFileName::Mkdir(folder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileDialog save(this, "Receive " + utf8(f.name) + " from " + utf8(f.from), folder, utf8(f.name),
                      "All files|*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (save.ShowModal() != wxID_OK) return;
    std::string error;
    if (!broadcastFiles_.receive(id, std::string(save.GetPath().utf8_str()), HostConnection::nowMs(), error))
    {
        note_ = error;
        showStatus();
    }
}

void MainFrame::broadcastFileAction(uint64_t id, const std::string& action)
{
    broadcast::BroadcastFile f;
    uint64_t now = HostConnection::nowMs();
    if (!broadcastFiles_.find(id, now, f)) return;
    if (action == "receive")
        receiveFile(id);
    else if (action == "reject")
        rejectFile(id);
    else if (action == "stopasking")
        broadcastFiles_.stop(id, now);
    else if (action == "askagain")
        askAgainOnBroadcast(id);
    else if (action == "stop")
        broadcastFiles_.cancel(id);
    else if (action == "keep")
        broadcastFiles_.keepOpen(id, std::max(f.repairsUntilMs, now) + 30 * 60 * 1000);
    else if (action == "keep2h")
        broadcastFiles_.keepOpen(id, std::max(f.repairsUntilMs, now) + 2 * 60 * 60 * 1000);
    else if (action == "keepuntil")
        keepRepairsUntil(id, f.repairsUntilMs);
    else if (action == "close")
        broadcastFiles_.closeRepairs(id);
    else if (action == "open" && !f.path.empty())
        openWithDesktop(f.path);
    else if (action == "folder" && !f.path.empty())
        openWithDesktop(std::string(wxFileName(utf8(f.path)).GetPath().utf8_str()));
    onBroadcastFiles();
    host_->flush();
    refreshFiles();
}

void MainFrame::rejectFile(uint64_t id)
{
    broadcast::BroadcastFile f;
    if (!broadcastFiles_.find(id, HostConnection::nowMs(), f)) return;
    wxString what = utf8(f.name) + " from " + utf8(f.from);
    wxString kept = f.held == 0 ? wxString()
                                : wxString::Format(" The %d of %d pieces here are thrown away.", (int)f.held,
                                                   (int)f.pieces);
    wxMessageDialog ask(this, "Reject " + what + "?" + kept + " Ragtime stops asking for it, and it can't be "
                              "received again unless " + utf8(f.from) + " broadcasts it again.",
                        "Reject file", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    ask.SetYesNoLabels("Reject", "Keep it");
    if (ask.ShowModal() != wxID_YES) return;
    broadcastFiles_.ignore(id);
}

void MainFrame::askAgainOnBroadcast(uint64_t id)
{
    // The same file carries on where it was saved to, or would have been.
    broadcast::BroadcastFile f;
    uint64_t now = HostConnection::nowMs();
    if (!broadcastFiles_.find(id, now, f)) return;
    std::string error;
    if (!f.path.empty() && broadcastFiles_.receive(id, "", now, error)) return;
    receiveFile(id);
}

void MainFrame::keepRepairsUntil(uint64_t id, uint64_t untilMs)
{
    // A local time today, or tomorrow if that has gone by: "18:30".
    wxDateTime shown = untilMs != 0 ? wxDateTime((wxLongLong)(long long)untilMs) : wxDateTime::Now();
    wxTextEntryDialog ask(this, "Keep repairs open until (your local time, such as 18:30):", "Keep repairs open",
                          shown.Format("%H:%M"));
    if (ask.ShowModal() != wxID_OK) return;
    wxDateTime at = wxDateTime::Now();
    wxString::const_iterator end;
    if (!at.ParseTime(ask.GetValue().Trim().Trim(false), &end))
    {
        wxMessageBox("\"" + ask.GetValue() + "\" isn't a time of day, such as 18:30.", "Keep repairs open",
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (at <= wxDateTime::Now()) at += wxDateSpan::Day();
    uint64_t ms = (uint64_t)at.GetValue().GetValue();
    broadcastFiles_.keepOpen(id, ms);
}

void MainFrame::broadcastFileMenu(const files::FileRow& row, wxMenu& menu)
{
    FileCard card;
    if (!fileCard(row.id, card)) return;
    const auto& actions = card.menu.empty() ? card.actions : card.menu;
    if (!card.title.empty())
    {
        menu.Append(ID_BROADCAST_FILE_TITLE, utf8(card.title));
        menu.Enable(ID_BROADCAST_FILE_TITLE, false);
    }
    uint64_t id = row.id;
    for (size_t i = 0; i < actions.size() && i < BROADCAST_FILE_ACTIONS; i++)
    {
        int itemId = ID_BROADCAST_FILE_ACTION + (int)i;
        menu.Append(itemId, utf8(actions[i].second));
        std::string action = actions[i].first;
        menu.Bind(wxEVT_MENU, [this, id, action](wxCommandEvent&) {
            CallAfter([this, id, action]() { broadcastFileAction(id, action); });
        }, itemId);
    }
}

void MainFrame::setAutoReceive(bool on)
{
    broadcastFiles_.setAutoReceive(on ? receivedFolder() : std::string());
    GetMenuBar()->Check(ID_AUTO_RECEIVE, on);
    saveBool(keys::AUTO_RECEIVE_BROADCAST, on);
}

void MainFrame::onBroadcastFiles()
{
    using Type = broadcast::BroadcastFiles::Event::Type;
    uint64_t now = HostConnection::nowMs();
    for (const broadcast::BroadcastFiles::Event& e : broadcastFiles_.takeEvents())
    {
        broadcast::BroadcastFile f;
        if (!broadcastFiles_.find(e.id, now, f)) continue;
        switch (e.type)
        {
            case Type::Announced:
                chat_.addFile(f.id, false, f.from, f.name, now);
                note_ = f.from + " is broadcasting " + f.name;
                if (!IsActive()) RequestUserAttention(wxUSER_ATTENTION_INFO);
                break;
            case Type::Sending:
                chat_.addFile(f.id, true, f.from, f.name, now);
                note_ = "Broadcasting " + f.name;
                break;
            case Type::Saved:
                note_ = "Saved " + f.name + " from " + f.from;
                chat_.fileEnded(f.id, "Saved");
                break;
            case Type::Ended:
                if (f.outgoing) note_ = f.name + ": " + broadcast::stateLabel(f.state);
                chat_.fileEnded(f.id, broadcast::stateLabel(f.state));
                break;
        }
    }
    for (files::FinishedFile& done : broadcastFiles_.takeFinished())
    {
        note_ = done.name + (done.outgoing ? " to " : " from ") + done.peer + ": " + files::resultLabel(done);
        history_.add(std::move(done));
    }
    if (broadcastFiles_.version() != broadcastFilesVersion_)
    {
        broadcastFilesVersion_ = broadcastFiles_.version();
        chat_.fileChanged();
        refreshMessages();
        showStatus();
    }
    saveBroadcastState();
}

void MainFrame::saveBroadcastState()
{
    broadcast::Seals::Chain chain = seals_.chain();
    if (chain.number == savedSeal_) return;
    savedSeal_ = chain.number;
    saveSealChain(chat_.settings().callsign, chain);
    saveNewestItem(chat_.settings().callsign, chat_.newestItem());
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
    // The gearshift's width only picks the modes gears come from; it never
    // limits data2g-host. That is the 500 Hz switch's job (setNarrow).
    saveGearshift(gearshift_);
    showGear();
}

void MainFrame::showGear()
{
    std::optional<gearbox::Gear> gear = gearshift_.current();
    const wxString width = wxString(" Up to ") + gearbox::widthLabel(gearshift_.effectiveWidth())
                           + ". Opens the gearshift: speed, width and path.";
    if (!gear)
    {
        broadcast_->setGearLabel("host", "No gears yet: the gearshift says why. Until then broadcasts go in "
                                         "the group's mode as data2g-host has it." + width);
        return;
    }
    wxString tip = "Gear " + utf8(gear->name.empty() ? gear->mode : gear->name + ", Data2G's " + gear->mode)
                   + ": " + utf8(gearbox::formatSpeed(gear->bitsPerSecond)) + ", needs "
                   + utf8(gearbox::formatDb(gear->threshold)) + " on a "
                   + gearbox::pathLabel(gearshift_.path()) + " path." + width;
    if (!link_.modes().empty() && broadcastMode().empty())
        tip += " data2g-host doesn't offer this mode, so broadcasts go in the group's mode.";
    broadcast_->setGearLabel(utf8(gear->name.empty() ? gear->mode : gear->name), tip);
}

void MainFrame::updatePathSnr(uint64_t nowMs)
{
    std::optional<broadcast::PathReport> path = stations_.weakestPath(nowMs);
    std::optional<double> dB = path ? std::optional<double>(path->dB) : std::nullopt;
    size_t bursts = path ? path->bursts : 0;
    std::string from = path ? path->call : std::string();
    if (dB == gearshift_.pathSnr() && bursts == gearshift_.snrBursts() && from == gearshift_.snrFrom()) return;
    gearshift_.setPathSnr(dB, bursts, from);
    showGear();
}

void MainFrame::setNarrow(bool narrow)
{
    gearshift_.setNarrowOnly(narrow);
    saveBool(keys::NARROW_ONLY, narrow);
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
