#include "SettingsDialog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/filepicker.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

#include "Html.h"
#include "ragtime/globus/Globe.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Fields.h"
#include "ragtime/radio/Bands.h"

using namespace ragtime;

namespace
{

std::string typed(const wxTextCtrl* control)
{
    return std::string(control->GetValue().Strip(wxString::both).utf8_str());
}

std::string typed(const wxComboBox* control)
{
    return std::string(control->GetValue().Strip(wxString::both).utf8_str());
}

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

// The serial choices, in their menus' order, the rig's own first.
const int BAUDS[] = {0, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
const int DATA_BITS[] = {0, 7, 8};
const int STOP_BITS[] = {0, 1, 2};
const radio::Parity PARITIES[] = {radio::Parity::Default, radio::Parity::None, radio::Parity::Odd,
                                  radio::Parity::Even, radio::Parity::Mark, radio::Parity::Space};
const radio::Handshake HANDSHAKES[] = {radio::Handshake::Default, radio::Handshake::None,
                                       radio::Handshake::XonXoff, radio::Handshake::Hardware};
const radio::LineState LINES[] = {radio::LineState::Default, radio::LineState::On, radio::LineState::Off};
const radio::PttMethod PTTS[] = {radio::PttMethod::CatData, radio::PttMethod::CatMic, radio::PttMethod::Dtr,
                                 radio::PttMethod::Rts};

template <typename T, size_t N>
int indexOf(const T (&values)[N], T value)
{
    for (size_t i = 0; i < N; i++)
    {
        if (values[i] == value) return (int)i;
    }
    return 0;
}

template <typename T, size_t N>
T valueAt(const T (&values)[N], const wxChoice* choice)
{
    int i = choice->GetSelection();
    return i >= 0 && (size_t)i < N ? values[i] : values[0];
}

template <size_t N>
wxChoice* numberChoice(wxWindow* parent, const int (&values)[N], int value)
{
    auto* choice = new wxChoice(parent, wxID_ANY);
    for (int v : values) choice->Append(v == 0 ? wxString("Rig's own") : wxString::Format("%d", v));
    choice->SetSelection(indexOf(values, value));
    return choice;
}

wxChoice* wordChoice(wxWindow* parent, const std::vector<wxString>& words, int selection)
{
    auto* choice = new wxChoice(parent, wxID_ANY);
    for (const wxString& word : words) choice->Append(word);
    choice->SetSelection(selection);
    return choice;
}

} // namespace

SettingsDialog::SettingsDialog(wxWindow* parent, const StationPrefs& prefs,
                               const std::vector<std::pair<std::string, std::string>>& gears,
                               std::function<std::string(const radio::RigSettings&)> openRig, Page page,
                               std::optional<double> txLevel)
    : wxDialog(parent, wxID_ANY, "Settings")
    , openRig_(std::move(openRig))
    , testTimer_(this)
    , rigctldTimer_(this)
{
    auto* book = new wxNotebook(this, wxID_ANY);
    book->AddPage(stationPage(book, prefs, gears, txLevel), "Station");
    book->AddPage(rigPage(book, prefs), "Rig");
    book->SetSelection(page == Page::Rig ? 1 : 0);

    problem_ = new wxStaticText(this, wxID_ANY, "");
    problem_->SetForegroundColour(badText());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(problem_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
    SetSizer(sizer);
    // Room for the longest problem line, so the dialog never jumps.
    problem_->SetMinSize(wxSize(FromDIP(380), problem_->GetCharHeight()));
    Fit();

    auto changed = [this](wxCommandEvent&) { check(); };
    callsign_->Bind(wxEVT_TEXT, changed);
    autoAccept_->Bind(wxEVT_TEXT, changed);
    locator_->Bind(wxEVT_TEXT, changed);
    commandPort_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { check(); });
    repairCap_->Bind(wxEVT_CHECKBOX, changed);
    for (wxRadioButton* r : {noRig_, useRigctld_, useHamlib_}) r->Bind(wxEVT_RADIOBUTTON, changed);
    maker_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        fillModels(0);
        modelPicked();
    });
    model_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { modelPicked(); });
    port_->Bind(wxEVT_TEXT, changed);
    port_->Bind(wxEVT_COMBOBOX, changed);
    backend_->Bind(wxEVT_TEXT, changed);
    keyRig_->Bind(wxEVT_CHECKBOX, changed);
    ptt_->Bind(wxEVT_CHOICE, changed);
    pttPort_->Bind(wxEVT_TEXT, changed);
    testButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { test(); });
    rigctldTest_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { testRigctld(); });
    Bind(wxEVT_TIMER, &SettingsDialog::onTestTimer, this);
    modelPicked();
    if (page == Page::Station) callsign_->SetFocus();
}

SettingsDialog::~SettingsDialog()
{
    testTimer_.Stop();
    rigctldTimer_.Stop();
}

//-------------------------------------------------------------------------
// Station
//-------------------------------------------------------------------------

wxWindow* SettingsDialog::stationPage(wxNotebook* book, const StationPrefs& prefs,
                                      const std::vector<std::pair<std::string, std::string>>& gears,
                                      std::optional<double> txLevel)
{
    auto* page = new wxPanel(book);
    // Controls go inside their static boxes, as wx wants them.
    auto* station = new wxStaticBoxSizer(wxVERTICAL, page, "Station");
    wxWindow* box = station->GetStaticBox();
    callsign_ = new wxTextCtrl(box, wxID_ANY, wxString::FromUTF8(prefs.callsign));
    callsign_->SetHint("AG7EW");
    callsign_->SetToolTip("Sent in every record, and to data2g-host as MYCALL");
    locator_ = new wxTextCtrl(box, wxID_ANY, wxString::FromUTF8(prefs.locator));
    locator_->SetHint("CN87ux");
    locator_->SetToolTip("Your Maidenhead locator, 4 or 6 characters: sent in your check-ins, and the "
                         "start of every path on Globus");
    auto* stationGrid = new wxFlexGridSizer(2, 6, 10);
    stationGrid->AddGrowableCol(1);
    stationGrid->Add(new wxStaticText(box, wxID_ANY, "Callsign"), 0, wxALIGN_CENTER_VERTICAL);
    stationGrid->Add(callsign_, 1, wxEXPAND);
    stationGrid->Add(new wxStaticText(box, wxID_ANY, "Locator"), 0, wxALIGN_CENTER_VERTICAL);
    stationGrid->Add(locator_, 1, wxEXPAND);
    station->Add(stationGrid, 0, wxEXPAND | wxALL, 6);

    auto* host = new wxStaticBoxSizer(wxVERTICAL, page, "data2g-host");
    box = host->GetStaticBox();
    host_ = new wxTextCtrl(box, wxID_ANY, wxString::FromUTF8(prefs.host));
    host_->SetToolTip("The computer data2g-host runs on. Ragtime never starts it: you run it.");
    kissPort_ = new wxSpinCtrl(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535,
                               prefs.kissPort);
    commandPort_ = new wxSpinCtrl(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65534,
                                  prefs.commandPort);
    dataPort_ = new wxStaticText(box, wxID_ANY, "");
    dataPort_->SetForegroundColour(secondaryText());
    auto* hostGrid = new wxFlexGridSizer(2, 6, 10);
    hostGrid->AddGrowableCol(1);
    hostGrid->Add(new wxStaticText(box, wxID_ANY, "Address"), 0, wxALIGN_CENTER_VERTICAL);
    hostGrid->Add(host_, 1, wxEXPAND);
    hostGrid->Add(new wxStaticText(box, wxID_ANY, "KISS port"), 0, wxALIGN_CENTER_VERTICAL);
    hostGrid->Add(kissPort_, 0);
    hostGrid->Add(new wxStaticText(box, wxID_ANY, "Command port"), 0, wxALIGN_CENTER_VERTICAL);
    hostGrid->Add(commandPort_, 0);
    hostGrid->AddSpacer(0);
    hostGrid->Add(dataPort_, 0);
    // The host's transmit level (TXPOWER, Data2G PR #122): read from the
    // host as the dialog opens, set on it with OK, never kept here.
    hostTxLevel_ = txLevel;
    txLevel_ = new wxSpinCtrlDouble(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                                    link::HostLink::TX_POWER_MIN_DB, link::HostLink::TX_POWER_MAX_DB,
                                    txLevel ? *txLevel : 0.0, 0.5);
    txLevel_->SetDigits(1);
    txLevel_->Enable(txLevel.has_value());
    txLevel_->SetToolTip(txLevel ? "data2g-host's transmit audio level (TXPOWER), in dB: 0 puts a burst's peak at "
                                   "full scale. It is data2g-host's own, as it answered just now; OK sets it there "
                                   "at once, and data2g-host keeps it after Ragtime quits. Auto ALC, on the "
                                   "connection bar, only ever turns it down from here."
                                 : "data2g-host's transmit audio level (TXPOWER). It needs data2g-host connected, "
                                   "and one from Data2G PR #122 on; until then set its --output-volume.");
    auto* txRow = new wxBoxSizer(wxHORIZONTAL);
    txRow->Add(txLevel_, 0, wxALIGN_CENTER_VERTICAL);
    auto* txNote = new wxStaticText(box, wxID_ANY, txLevel ? "dB, data2g-host's own" : "dB (data2g-host can't say)");
    txNote->SetForegroundColour(secondaryText());
    txRow->Add(txNote, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    hostGrid->Add(new wxStaticText(box, wxID_ANY, "TX level"), 0, wxALIGN_CENTER_VERTICAL);
    hostGrid->Add(txRow, 0);
    host->Add(hostGrid, 0, wxEXPAND | wxALL, 6);

    auto* files = new wxStaticBoxSizer(wxVERTICAL, page, "Files");
    box = files->GetStaticBox();
    received_ = new wxDirPickerCtrl(box, wxID_ANY, wxString::FromUTF8(prefs.receivedFolder), "Where received files go",
                                    wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
    received_->SetToolTip("Save puts a file offered in a session here; a part file waits beside it while it comes");
    wxString calls;
    for (const std::string& call : prefs.autoAccept) calls << (calls.empty() ? "" : " ") << wxString::FromUTF8(call);
    autoAccept_ = new wxTextCtrl(box, wxID_ANY, calls);
    autoAccept_->SetHint("W1AW KC2G");
    autoAccept_->SetToolTip("Files these stations offer in a session are saved in the received folder without "
                            "asking, under the name offered (numbered if it's taken). Callsigns, separated by spaces");
    auto* filesGrid = new wxFlexGridSizer(2, 6, 10);
    filesGrid->AddGrowableCol(1);
    filesGrid->Add(new wxStaticText(box, wxID_ANY, "Received files"), 0, wxALIGN_CENTER_VERTICAL);
    filesGrid->Add(received_, 1, wxEXPAND);
    filesGrid->Add(new wxStaticText(box, wxID_ANY, "Save without asking from"), 0, wxALIGN_CENTER_VERTICAL);
    filesGrid->Add(autoAccept_, 1, wxEXPAND);
    files->Add(filesGrid, 0, wxEXPAND | wxALL, 6);

    auto* broadcast = new wxStaticBoxSizer(wxVERTICAL, page, "Broadcast");
    box = broadcast->GetStaticBox();
    sealChat_ = new wxCheckBox(box, wxID_ANY, "Seal my broadcast messages");
    sealChat_->SetValue(prefs.sealChat);
    sealChat_->SetToolTip("A minute after you stop sending, a seal goes out listing your messages, so a station "
                          "that missed one can ask for it again. It is a transmission of its own: 10 to 60 s "
                          "of airtime on a poor link. Files you broadcast are always sealed.");
    broadcast->Add(sealChat_, 0, wxALL, 6);
    slowRepair_ = new wxSpinCtrl(box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                                 SLOW_REPAIR_MIN_MINUTES, SLOW_REPAIR_MAX_MINUTES, prefs.slowRepairMinutes);
    const char* slowTip = "A station that asks for repairs says how well it hears you, and what it lacks goes again "
                          "in a gear it can copy. Repairs in gears slower than the round's own take at most this much "
                          "airtime a round; the rest goes in the next round, so others get a word in between.";
    slowRepair_->SetToolTip(slowTip);
    auto* slowRow = new wxBoxSizer(wxHORIZONTAL);
    auto* slowLabel = new wxStaticText(box, wxID_ANY, "Maximum time spent in repairs per round:");
    slowLabel->SetToolTip(slowTip);
    slowRow->Add(slowLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    slowRow->Add(slowRepair_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    slowRow->Add(new wxStaticText(box, wxID_ANY, "minutes"), 0, wxALIGN_CENTER_VERTICAL);
    broadcast->Add(slowRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    // Andrew's floor and cap (2026-10-25).
    repairFloor_ = new wxChoice(box, wxID_ANY);
    repairFloor_->Append("None: as slow as a station needs");
    floorModes_.push_back("");
    bool listed = prefs.repairFloor.empty();
    for (const auto& gear : gears)
    {
        repairFloor_->Append(wxString::FromUTF8(gear.second));
        floorModes_.push_back(gear.first);
        listed = listed || gear.first == prefs.repairFloor;
    }
    // Chosen at another width: kept, under its own name.
    if (!listed)
    {
        repairFloor_->Append(wxString::FromUTF8(prefs.repairFloor));
        floorModes_.push_back(prefs.repairFloor);
    }
    for (size_t i = 0; i < floorModes_.size(); i++)
    {
        if (floorModes_[i] == prefs.repairFloor) repairFloor_->SetSelection((int)i);
    }
    const char* floorTip = "Repairs never go in a slower gear than this, even for a station that says it hears "
                           "you worse: it still gains from what it copies, and the rest of the net isn't held up "
                           "by the slowest gear. A file's own Repair gear (its right-click menu) goes before this.";
    repairFloor_->SetToolTip(floorTip);
    auto* floorRow = new wxBoxSizer(wxHORIZONTAL);
    auto* floorLabel = new wxStaticText(box, wxID_ANY, "Slowest repair gear:");
    floorLabel->SetToolTip(floorTip);
    floorRow->Add(floorLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    floorRow->Add(repairFloor_, 1, wxALIGN_CENTER_VERTICAL);
    broadcast->Add(floorRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    repairCap_ = new wxCheckBox(box, wxID_ANY, "Close a file's repairs after");
    repairCap_->SetValue(prefs.repairCap);
    repairCapMinutes_ = new wxSpinCtrl(box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                       wxSP_ARROW_KEYS, REPAIR_CAP_MIN_MINUTES, REPAIR_CAP_MAX_MINUTES,
                                       prefs.repairCapMinutes);
    const char* capTip = "Once a file's repairs have taken this much airtime in all, they close, and stations "
                         "that ask hear that they have. Re-open repairs on its right-click menu gives it as much "
                         "again.";
    repairCap_->SetToolTip(capTip);
    repairCapMinutes_->SetToolTip(capTip);
    auto* capRow = new wxBoxSizer(wxHORIZONTAL);
    capRow->Add(repairCap_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    capRow->Add(repairCapMinutes_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    capRow->Add(new wxStaticText(box, wxID_ANY, "minutes of repair airtime"), 0, wxALIGN_CENTER_VERTICAL);
    broadcast->Add(capRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    // Two columns, so it stays short enough for a 1366x768 screen: the
    // station and its connection on the left, what it sends on the right.
    auto* left = new wxBoxSizer(wxVERTICAL);
    left->Add(station, 0, wxEXPAND | wxTOP, 12);
    left->Add(host, 0, wxEXPAND | wxTOP, 12);
    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(files, 0, wxEXPAND | wxTOP, 12);
    right->Add(broadcast, 0, wxEXPAND | wxTOP, 12);
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    columns->Add(left, 1, wxEXPAND | wxLEFT, 12);
    columns->Add(right, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(columns, 0, wxEXPAND | wxBOTTOM, 12);
    page->SetSizer(sizer);
    return page;
}

//-------------------------------------------------------------------------
// Rig
//-------------------------------------------------------------------------

wxWindow* SettingsDialog::rigPage(wxNotebook* book, const StationPrefs& prefs)
{
    const radio::RigSettings& r = prefs.rig;
    auto* page = new wxPanel(book);

    // How Ragtime knows the radio.
    noRig_ = new wxRadioButton(page, wxID_ANY, "No rig: Ragtime doesn't know the band", wxDefaultPosition,
                               wxDefaultSize, wxRB_GROUP);
    useRigctld_ = new wxRadioButton(page, wxID_ANY, "Read the dial from rigctld, already running");
    useRigctld_->SetToolTip("Once a second Ragtime asks rigctld the radio's frequency, and while data2g-host "
                            "transmits the rig's ALC, SWR and power, and nothing else: it never tunes the radio, "
                            "keys it or sets anything, since rigctld is often shared (fldigi). Ragtime never "
                            "starts rigctld: you run it.");
    rigctldHost_ = new wxTextCtrl(page, wxID_ANY, utf8(r.rigctldHost), wxDefaultPosition, wxSize(FromDIP(120), -1));
    rigctldHost_->SetToolTip("The computer rigctld runs on");
    rigctldPort_ = new wxSpinCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535,
                                  r.rigctldPort);
    rigctldPort_->SetToolTip("rigctld's port, 4532 unless you started it with another");
    rigctldTest_ = new wxButton(page, wxID_ANY, "Test");
    rigctldTest_->SetToolTip("Connects to rigctld as Ragtime would, trying IPv4 and IPv6, asks which transmit "
                             "meters the rig reports, and asks the dial frequency once. It sets nothing.");
    rigctldResult_ = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    useHamlib_ = new wxRadioButton(page, wxID_ANY, "Open the rig with Hamlib (" + utf8(rig::hamlibVersion()) + ")");
    useHamlib_->SetToolTip("Ragtime opens the rig itself, as other ham programs do, and reads its dial. It keys the "
                           "rig only if you tick Key the rig below. Nothing else may hold the rig's port meanwhile, "
                           "unless its driver can be opened twice.");
    noRig_->SetValue(r.source == radio::RigSource::None);
    useRigctld_->SetValue(r.source == radio::RigSource::Rigctld);
    useHamlib_->SetValue(r.source == radio::RigSource::Hamlib && rig::available());
    if (!rig::available())
    {
        useHamlib_->Disable();
        if (r.source == radio::RigSource::Hamlib) noRig_->SetValue(true);
    }

    auto* rigctldRow = new wxBoxSizer(wxHORIZONTAL);
    rigctldRow->Add(useRigctld_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    rigctldRow->Add(new wxStaticText(page, wxID_ANY, "at"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigctldRow->Add(rigctldHost_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigctldRow->Add(new wxStaticText(page, wxID_ANY, "port"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigctldRow->Add(rigctldPort_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    rigctldRow->Add(rigctldTest_, 0, wxALIGN_CENTER_VERTICAL);
    // The test's answer, under the row, lined up with the radio button's text.
    auto* rigctldResultRow = new wxBoxSizer(wxHORIZONTAL);
    rigctldResultRow->AddSpacer(FromDIP(22));
    rigctldResultRow->Add(rigctldResult_, 1, wxALIGN_CENTER_VERTICAL | wxTOP, 4);

    // Hamlib: the rig, its port, the serial settings, the backend's own.
    auto* hamlib = new wxStaticBoxSizer(wxVERTICAL, page, "Hamlib");
    wxWindow* box = hamlib->GetStaticBox();
    models_ = rig::models();
    for (const rig::Model& m : models_)
    {
        if (makers_.empty() || makers_.back() != m.maker) makers_.push_back(m.maker);
    }
    maker_ = new wxChoice(box, wxID_ANY);
    for (const std::string& maker : makers_) maker_->Append(utf8(maker));
    model_ = new wxChoice(box, wxID_ANY);
    model_->SetMinSize(wxSize(FromDIP(220), -1));
    // The saved model's maker, or the first.
    int makerAt = 0;
    for (const rig::Model& m : models_)
    {
        if (m.id != r.model) continue;
        for (size_t i = 0; i < makers_.size(); i++)
        {
            if (makers_[i] == m.maker) makerAt = (int)i;
        }
    }
    if (!makers_.empty()) maker_->SetSelection(makerAt);
    if (r.model != 0 && !rig::model(r.model)) unknownModel_ = r.model;
    fillModels(r.model);

    port_ = new wxComboBox(box, wxID_ANY, utf8(r.port));
    for (const std::string& p : rig::serialPorts()) port_->Append(utf8(p));
    port_->SetMinSize(wxSize(FromDIP(260), -1));

    baud_ = numberChoice(box, BAUDS, r.baud);
    dataBits_ = numberChoice(box, DATA_BITS, r.dataBits);
    stopBits_ = numberChoice(box, STOP_BITS, r.stopBits);
    parity_ = wordChoice(box, {"Rig's own", "None", "Odd", "Even", "Mark", "Space"}, indexOf(PARITIES, r.parity));
    handshake_ = wordChoice(box, {"Rig's own", "None", "XON/XOFF", "Hardware"}, indexOf(HANDSHAKES, r.handshake));
    dtr_ = wordChoice(box, {"Left alone", "On", "Off"}, indexOf(LINES, r.dtr));
    rts_ = wordChoice(box, {"Left alone", "On", "Off"}, indexOf(LINES, r.rts));
    dtr_->SetToolTip("Held on or off while the rig is open: some CAT interfaces draw their power from it");
    rts_->SetToolTip(dtr_->GetToolTipText());
    backend_ = new wxTextCtrl(box, wxID_ANY, utf8(r.backendConf));
    backend_->SetHint("civaddr=0x88");
    poll_ = new wxSpinCtrl(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                           radio::POLL_MIN_SECONDS, radio::POLL_MAX_SECONDS, r.pollSeconds);
    poll_->SetToolTip("Never while keyed: some radios answer CAT slowly while they transmit");
    testButton_ = new wxButton(box, wxID_ANY, "Test");
    testButton_->SetToolTip("Opens the rig with these settings and reads its dial. It never keys the rig.");
    testResult_ = new wxStaticText(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);

    auto* grid = new wxFlexGridSizer(2, 6, 10);
    grid->AddGrowableCol(1);
    auto* rigRow = new wxBoxSizer(wxHORIZONTAL);
    rigRow->Add(maker_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigRow->Add(model_, 1, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(box, wxID_ANY, "Rig"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(rigRow, 1, wxEXPAND);
    grid->Add(new wxStaticText(box, wxID_ANY, "Port"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(port_, 1, wxEXPAND);
    grid->Add(new wxStaticText(box, wxID_ANY, "Serial"), 0, wxALIGN_TOP | wxTOP, 4);
    auto* serial = new wxFlexGridSizer(8, 6, 6);
    auto addSerial = [&](const char* label, wxWindow* control) {
        auto* text = new wxStaticText(box, wxID_ANY, label);
        serial->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, serial->GetItemCount() % 8 == 0 ? 0 : 6);
        serial->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        serial_.push_back(text);
        serial_.push_back(control);
    };
    addSerial("Baud", baud_);
    addSerial("Data bits", dataBits_);
    addSerial("Stop bits", stopBits_);
    addSerial("Parity", parity_);
    addSerial("Handshake", handshake_);
    addSerial("DTR", dtr_);
    addSerial("RTS", rts_);
    grid->Add(serial, 0);
    grid->Add(new wxStaticText(box, wxID_ANY, "Backend settings"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(backend_, 1, wxEXPAND);
    auto* pollRow = new wxBoxSizer(wxHORIZONTAL);
    pollRow->Add(poll_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    pollRow->Add(new wxStaticText(box, wxID_ANY, "s"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 18);
    pollRow->Add(testButton_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    pollRow->Add(testResult_, 1, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(box, wxID_ANY, "Read the dial every"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(pollRow, 1, wxEXPAND);
    hamlib->Add(grid, 0, wxEXPAND | wxALL, 6);

    // Keying, off by default: Andrew's suggestion (2026-10-26) for rigs
    // whose drivers can't be opened by two programs.
    keyRig_ = new wxCheckBox(box, wxID_ANY, "Key the rig when data2g-host keys");
    keyRig_->SetValue(r.keyRig);
    keyRig_->SetToolTip("data2g-host says PTT ON on its command port when it starts a burst and PTT OFF when it "
                        "ends, even when it keys nothing itself. With this ticked Ragtime keys the rig at PTT ON "
                        "and unkeys it at PTT OFF.");
    ptt_ = new wxChoice(box, wxID_ANY);
    for (radio::PttMethod m : PTTS) ptt_->Append(radio::pttLabel(m));
    ptt_->SetSelection(indexOf(PTTS, r.ptt));
    ptt_->SetToolTip("CAT keys the rig by a command on its port; data audio takes the sound from the rear data "
                     "port (USB on most rigs). DTR and RTS key it with a line of a serial port, the rig's own or "
                     "another.");
    pttPort_ = new wxComboBox(box, wxID_ANY, utf8(r.pttPort));
    for (const std::string& p : rig::serialPorts()) pttPort_->Append(utf8(p));
    pttPort_->SetHint("the rig's port");
    pttPort_->SetToolTip("The serial port whose DTR or RTS line keys the rig. Empty: the rig's own port.");
    auto* pttText = new wxStaticText(box, wxID_ANY, "PTT by");
    auto* pttPortText = new wxStaticText(box, wxID_ANY, "on");
    auto* hint = new wxStaticText(box, wxID_ANY,
                                  "Start data2g-host with --no-rig, so it leaves the rig to Ragtime, and "
                                  "--ptt-on-delay-ms 300, so the rig is keyed before its audio starts. Ragtime "
                                  "unkeys at PTT OFF, if the link to data2g-host drops, if the rig fails, when it "
                                  "closes, and after 180 s keyed in any case.");
    hint->SetForegroundColour(secondaryText());
    hint->Wrap(FromDIP(860));
    keying_ = {ptt_, pttPort_, pttText, pttPortText, hint};
    auto* pttRow = new wxBoxSizer(wxHORIZONTAL);
    pttRow->Add(pttText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    pttRow->Add(ptt_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    pttRow->Add(pttPortText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    pttRow->Add(pttPort_, 1, wxALIGN_CENTER_VERTICAL);
    hamlib->Add(keyRig_, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    hamlib->AddSpacer(4);
    hamlib->Add(pttRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 30);
    hamlib->AddSpacer(4);
    hamlib->Add(hint, 0, wxLEFT | wxRIGHT, 30);

    // The SWR trip, off by default (Jeff, 2026-10-26), for rigctld and
    // Hamlib alike; only Hamlib, where Ragtime opens the rig, may unkey and
    // turn down.
    swrTrip_ = new wxCheckBox(page, wxID_ANY, "Over 3:1 SWR, stop data2g-host; with Hamlib also unkey the rig "
                                              "and turn its power to 0");
    swrTrip_->SetValue(r.swrTrip);
    swrTrip_->SetToolTip("While data2g-host transmits, Ragtime reads the rig's SWR, through rigctld or Hamlib. "
                         "With this ticked, two readings in a row over 3:1 and Ragtime tells data2g-host to stop "
                         "(ABORT, which also ends a session) and holds sending until you clear the trip "
                         "(Settings > Clear the SWR trip, or click the SWR bar). With Hamlib it also unkeys the "
                         "rig, whoever keyed it, and sets its power to 0, and never puts the power back: turn it "
                         "up on the rig. Through rigctld it sets nothing, since rigctld is often shared. For rigs "
                         "that report their SWR.");
    hamlib->AddSpacer(6);

    usSegmentsOnly_ = new wxCheckBox(page, wxID_ANY, "Receive only outside the US data segments");
    usSegmentsOnly_->SetValue(prefs.usSegmentsOnly);
    usSegmentsOnly_->SetToolTip("Data2G bursts are data, and US rules allow data only in parts of each band "
                                "(47 CFR 97.305). While this is ticked Ragtime hands data2g-host nothing to send, "
                                "and takes no calls, unless the dial is at least 3 kHz inside a data segment; nor "
                                "while it doesn't know the dial. It still receives. A session already up keeps "
                                "its link: data2g-host still acknowledges what it hears.");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(noRig_, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(rigctldRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(rigctldResultRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->Add(useHamlib_, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(hamlib, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(swrTrip_, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(usSegmentsOnly_, 0, wxALL, 12);
    page->SetSizer(sizer);
    return page;
}

void SettingsDialog::fillModels(int pick)
{
    model_->Clear();
    shown_.clear();
    int maker = maker_->GetSelection();
    int selection = -1;
    if (unknownModel_ != 0)
    {
        // Saved by a Ragtime whose Hamlib knew it: kept, by number.
        model_->Append(wxString::Format("Model %d (not in this Hamlib)", unknownModel_));
        shown_.push_back(models_.size());
        if (pick == unknownModel_) selection = 0;
    }
    for (size_t i = 0; maker >= 0 && i < models_.size(); i++)
    {
        const rig::Model& m = models_[i];
        if (m.maker != makers_[(size_t)maker]) continue;
        wxString label = utf8(m.name);
        if (m.status != "Stable") label << "  (" << utf8(m.status) << ")";
        if (m.id == pick) selection = (int)shown_.size();
        model_->Append(label);
        shown_.push_back(i);
    }
    // Otherwise the maker's first rig.
    if (selection < 0) selection = std::min((int)shown_.size() - 1, unknownModel_ != 0 ? 1 : 0);
    if (!shown_.empty()) model_->SetSelection(selection);
}

const rig::Model* SettingsDialog::pickedModel() const
{
    int i = model_->GetSelection();
    if (i < 0 || (size_t)i >= shown_.size() || shown_[(size_t)i] >= models_.size()) return nullptr;
    return &models_[shown_[(size_t)i]];
}

void SettingsDialog::modelPicked()
{
    // Another rig: the earlier one wasn't kept.
    if (unknownModel_ != 0 && pickedModel() != nullptr) unknownModel_ = 0;
    const rig::Model* m = pickedModel();
    wxString tip = "The backend's own settings, as name=value separated by spaces, as rigctl -C takes them.";
    if (m != nullptr)
    {
        std::vector<rig::BackendParam> params = rig::backendParams(m->id);
        if (!params.empty()) tip << "\n\nThe " << utf8(m->name) << "'s:";
        for (const rig::BackendParam& p : params)
        {
            tip << "\n" << utf8(p.name) << ": " << utf8(p.label);
            if (!p.fallback.empty()) tip << " (" << utf8(p.fallback) << ")";
        }
        model_->SetToolTip(wxString::Format("Hamlib model %d", m->id));
    }
    backend_->SetToolTip(tip);
    check();
}

radio::RigSettings SettingsDialog::rig() const
{
    radio::RigSettings r;
    r.source = useHamlib_->GetValue()    ? radio::RigSource::Hamlib
               : useRigctld_->GetValue() ? radio::RigSource::Rigctld
                                         : radio::RigSource::None;
    r.rigctldHost = typed(rigctldHost_);
    if (r.rigctldHost.empty()) r.rigctldHost = "127.0.0.1";
    r.rigctldPort = rigctldPort_->GetValue();
    const rig::Model* m = pickedModel();
    r.model = m != nullptr ? m->id : unknownModel_;
    r.port = typed(port_);
    r.baud = BAUDS[std::max(0, baud_->GetSelection())];
    r.dataBits = DATA_BITS[std::max(0, dataBits_->GetSelection())];
    r.stopBits = STOP_BITS[std::max(0, stopBits_->GetSelection())];
    r.parity = valueAt(PARITIES, parity_);
    r.handshake = valueAt(HANDSHAKES, handshake_);
    r.dtr = valueAt(LINES, dtr_);
    r.rts = valueAt(LINES, rts_);
    r.backendConf = std::string(backend_->GetValue().Strip(wxString::both).utf8_str());
    r.pollSeconds = poll_->GetValue();
    r.keyRig = keyRig_->GetValue();
    r.swrTrip = swrTrip_->GetValue();
    r.ptt = valueAt(PTTS, ptt_);
    r.pttPort = typed(pttPort_);
    return r;
}

void SettingsDialog::test()
{
    radio::RigSettings r = rig();
    std::string open = openRig_ ? openRig_(r) : std::string();
    if (!open.empty())
    {
        testResult_->SetLabel(utf8(open.substr(0, open.find('\n'))));
        testResult_->SetToolTip(utf8(open));
        return;
    }
    // A test only reads: it never keys, and names no PTT line.
    r.keyRig = false;
    testRig_ = std::make_unique<rig::HamlibRig>();
    testRig_->start(r);
    testTicks_ = 0;
    testResult_->SetForegroundColour(secondaryText());
    testResult_->SetLabel("Opening the rig...");
    testResult_->SetToolTip(wxString());
    testButton_->Disable();
    testTimer_.Start(200);
}

void SettingsDialog::onTestTimer(wxTimerEvent& event)
{
    if (&event.GetTimer() == &rigctldTimer_)
    {
        onRigctldTimer();
        return;
    }
    if (!testRig_) return;
    rig::HamlibRig::Status s = testRig_->status();
    bool done = false;
    wxString result;
    if (s.open && s.hz)
    {
        std::string band = radio::bandOf(*s.hz);
        result = utf8(s.rig + " answered: " + radio::formatMHz(*s.hz) + (band.empty() ? "" : ", " + band));
        testResult_->SetForegroundColour(goodText());
        done = true;
    }
    else if (!s.open && !s.problem.empty())
    {
        result = utf8(s.problem.substr(0, s.problem.find("; trying again")));
        testResult_->SetForegroundColour(badText());
        done = true;
    }
    else if (++testTicks_ > 75)
    {
        result = "No answer in 15 s";
        testResult_->SetForegroundColour(badText());
        done = true;
    }
    if (!done) return;
    testTimer_.Stop();
    testRig_->stop();
    testRig_.reset();
    testResult_->SetLabel(result);
    testResult_->SetToolTip(result);
    testButton_->Enable(useHamlib_->GetValue());
}

void SettingsDialog::testRigctld()
{
    radio::RigSettings r = rig();
    // A connection of its own, as the window's is: rigctld takes several.
    rigctldProbe_ = std::make_unique<RigConnection>(nullptr);
    rigctldProbe_->start(r.rigctldHost, r.rigctldPort);
    rigctldTicks_ = 0;
    rigctldResult_->SetForegroundColour(secondaryText());
    rigctldResult_->SetLabel("Connecting to rigctld at " + utf8(rigctldProbe_->address()) + "...");
    rigctldResult_->SetToolTip(wxString());
    rigctldTest_->Disable();
    rigctldTimer_.Start(200);
}

void SettingsDialog::onRigctldTimer()
{
    if (!rigctldProbe_) return;
    RigConnection& probe = *rigctldProbe_;
    std::string result;
    bool good = false;
    if (probe.up() && probe.dialHz())
    {
        uint64_t hz = *probe.dialHz();
        std::string band = radio::bandOf(hz);
        std::vector<std::string> meters;
        if (probe.readsAlc()) meters.push_back("ALC");
        if (probe.readsSwr()) meters.push_back("SWR");
        if (probe.readsPower()) meters.push_back("power");
        std::string reports = "no transmit meters";
        for (size_t i = 0; i < meters.size(); i++)
            reports = (i == 0 ? std::string() : reports + (i + 1 == meters.size() ? " and " : ", ")) + meters[i];
        result = "rigctld at " + probe.connectedTo() + " answered: " + radio::formatMHz(hz)
                 + (band.empty() ? "" : ", " + band) + "; the rig reports " + reports
                 + (probe.vfoMode() ? " (rigctld started with --vfo)" : "");
        good = true;
    }
    else if (probe.up() && !probe.problem().empty())
    {
        result = "Connected to " + probe.connectedTo() + ", but " + probe.problem();
    }
    else if (probe.failed())
    {
        result = probe.error();
        if (!result.empty()) result[0] = (char)std::toupper((unsigned char)result[0]);
    }
    else if (++rigctldTicks_ > 50)
    {
        result = probe.up() ? "Connected to " + probe.connectedTo() + ", but rigctld didn't tell the frequency in 10 s"
                            : "No answer from rigctld in 10 s";
    }
    else
    {
        return;
    }
    rigctldTimer_.Stop();
    rigctldProbe_.reset();
    rigctldResult_->SetForegroundColour(good ? goodText() : badText());
    rigctldResult_->SetLabel(utf8(result));
    rigctldResult_->SetToolTip(utf8(result));
    rigctldTest_->Enable(useRigctld_->GetValue());
}

//-------------------------------------------------------------------------
// Both
//-------------------------------------------------------------------------

std::optional<double> SettingsDialog::txLevel() const
{
    if (!hostTxLevel_ || !txLevel_->IsEnabled()) return std::nullopt;
    double dB = txLevel_->GetValue();
    if (std::fabs(dB - *hostTxLevel_) < 0.05) return std::nullopt;
    return dB;
}

StationPrefs SettingsDialog::prefs() const
{
    StationPrefs p;
    p.callsign = protocol::normalCallsign(typed(callsign_));
    p.locator = globus::tidyLocator(typed(locator_));
    p.host = typed(host_);
    if (p.host.empty()) p.host = "127.0.0.1";
    p.kissPort = kissPort_->GetValue();
    p.commandPort = commandPort_->GetValue();
    p.receivedFolder = std::string(received_->GetPath().Strip(wxString::both).utf8_str());
    wxStringTokenizer words(autoAccept_->GetValue(), ", ;");
    while (words.HasMoreTokens())
    {
        std::string call = protocol::normalCallsign(std::string(words.GetNextToken().utf8_str()));
        if (!call.empty()) p.autoAccept.push_back(call);
    }
    p.sealChat = sealChat_->GetValue();
    p.slowRepairMinutes = slowRepair_->GetValue();
    int floor = repairFloor_->GetSelection();
    p.repairFloor = floor > 0 && (size_t)floor < floorModes_.size() ? floorModes_[(size_t)floor] : std::string();
    p.repairCap = repairCap_->GetValue();
    p.repairCapMinutes = repairCapMinutes_->GetValue();
    p.rig = rig();
    p.usSegmentsOnly = p.readsDial() && usSegmentsOnly_->GetValue();
    return p;
}

void SettingsDialog::check()
{
    dataPort_->SetLabel(wxString::Format("Session data on %d", commandPort_->GetValue() + 1));
    repairCapMinutes_->Enable(repairCap_->GetValue());

    bool rigctld = useRigctld_->GetValue();
    bool hamlib = useHamlib_->GetValue();
    rigctldHost_->Enable(rigctld);
    rigctldPort_->Enable(rigctld);
    rigctldTest_->Enable(rigctld && !rigctldProbe_);
    const rig::Model* m = pickedModel();
    radio::PortType port = m != nullptr ? m->port : radio::PortType::Serial;
    maker_->Enable(hamlib);
    model_->Enable(hamlib);
    port_->Enable(hamlib && port != radio::PortType::None);
    port_->SetHint(port == radio::PortType::Network ? "host:port" : port == radio::PortType::None ? "" : "/dev/ttyUSB0");
    for (wxWindow* w : serial_) w->Enable(hamlib && port == radio::PortType::Serial);
    backend_->Enable(hamlib);
    poll_->Enable(hamlib);
    testButton_->Enable(hamlib && !testRig_);
    keyRig_->Enable(hamlib);
    swrTrip_->Enable(hamlib || rigctld);
    bool keying = hamlib && keyRig_->GetValue();
    for (wxWindow* w : keying_) w->Enable(keying);
    radio::PttMethod ptt = valueAt(PTTS, ptt_);
    pttPort_->Enable(keying && (ptt == radio::PttMethod::Dtr || ptt == radio::PttMethod::Rts));
    // The segment check needs the dial.
    usSegmentsOnly_->Enable(rigctld || hamlib);

    wxString problem;
    std::string call = typed(callsign_);
    std::string locator = typed(locator_);
    if (!call.empty() && protocol::normalCallsign(call).empty())
        problem = "A callsign is letters, digits, / and -, at most 10 characters.";
    else if (!locator.empty() && globus::tidyLocator(locator).empty())
        problem = "A locator is 4 or 6 characters, such as CN87 or CN87ux.";
    wxStringTokenizer words(autoAccept_->GetValue(), ", ;");
    while (problem.empty() && words.HasMoreTokens())
    {
        wxString word = words.GetNextToken();
        if (protocol::normalCallsign(std::string(word.utf8_str())).empty())
            problem = "\"" + word + "\" isn't a callsign: list the stations separated by spaces.";
    }
    if (problem.empty() && hamlib)
    {
        std::string why;
        if (m == nullptr && unknownModel_ == 0)
            why = "Pick the rig.";
        else
            radio::hamlibConf(rig(), port, why);
        problem = utf8(why);
    }
    problem_->SetLabel(problem);
    if (wxWindow* ok = FindWindow(wxID_OK)) ok->Enable(problem.empty());
    Layout();
}

bool SettingsDialog::TransferDataFromWindow()
{
    return problem_->GetLabel().empty();
}
