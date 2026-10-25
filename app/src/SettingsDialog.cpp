#include "SettingsDialog.h"

#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filepicker.h>
#include <wx/tokenzr.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "Html.h"
#include "ragtime/globus/Globe.h"
#include "ragtime/protocol/Fields.h"

namespace
{

std::string text(const wxTextCtrl* control)
{
    return std::string(control->GetValue().Strip(wxString::both).utf8_str());
}

} // namespace

SettingsDialog::SettingsDialog(wxWindow* parent, const StationPrefs& prefs,
                               const std::vector<std::pair<std::string, std::string>>& gears)
    : wxDialog(parent, wxID_ANY, "Station settings")
{
    // Controls go inside their static boxes, as wx wants them.
    auto* station = new wxStaticBoxSizer(wxVERTICAL, this, "Station");
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

    auto* host = new wxStaticBoxSizer(wxVERTICAL, this, "data2g-host");
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
    host->Add(hostGrid, 0, wxEXPAND | wxALL, 6);

    auto* files = new wxStaticBoxSizer(wxVERTICAL, this, "Files");
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

    auto* broadcast = new wxStaticBoxSizer(wxVERTICAL, this, "Broadcast");
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

    // The radio: read, never set. rigctld is often shared (fldigi), so
    // Ragtime asks it the frequency and nothing else.
    auto* radio = new wxStaticBoxSizer(wxVERTICAL, this, "Radio");
    box = radio->GetStaticBox();
    readDial_ = new wxCheckBox(box, wxID_ANY, "Read the dial from rigctld");
    readDial_->SetValue(prefs.readDial);
    readDial_->SetToolTip("Once a second Ragtime asks rigctld the radio's frequency, and nothing else: it never "
                          "tunes the radio or sets anything. The band goes in the path log beside every station "
                          "heard. Ragtime never starts rigctld: you run it.");
    rigHost_ = new wxTextCtrl(box, wxID_ANY, wxString::FromUTF8(prefs.rigHost), wxDefaultPosition,
                              wxSize(FromDIP(130), -1));
    rigHost_->SetToolTip("The computer rigctld runs on");
    rigPort_ = new wxSpinCtrl(box, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535,
                              prefs.rigPort);
    rigPort_->SetToolTip("rigctld's port, 4532 unless you started it with another");
    auto* rigRow = new wxBoxSizer(wxHORIZONTAL);
    rigRow->Add(readDial_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    rigRow->Add(new wxStaticText(box, wxID_ANY, "at"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigRow->Add(rigHost_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigRow->Add(new wxStaticText(box, wxID_ANY, "port"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    rigRow->Add(rigPort_, 0, wxALIGN_CENTER_VERTICAL);
    radio->Add(rigRow, 0, wxEXPAND | wxALL, 6);
    usSegmentsOnly_ = new wxCheckBox(box, wxID_ANY, "Receive only outside the US data segments");
    usSegmentsOnly_->SetValue(prefs.usSegmentsOnly);
    usSegmentsOnly_->SetToolTip("Data2G bursts are data, and US rules allow data only in parts of each band "
                                "(47 CFR 97.305). While this is ticked Ragtime hands data2g-host nothing to send, "
                                "and takes no calls, unless the dial is at least 3 kHz inside a data segment; nor "
                                "while it doesn't know the dial. It still receives. A session already up keeps "
                                "its link: data2g-host still acknowledges what it hears.");
    radio->Add(usSegmentsOnly_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    problem_ = new wxStaticText(this, wxID_ANY, "");
    problem_->SetForegroundColour(badText());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(station, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(files, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(broadcast, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(radio, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
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
    readDial_->Bind(wxEVT_CHECKBOX, changed);
    repairCap_->Bind(wxEVT_CHECKBOX, changed);
    check();
    callsign_->SetFocus();
}

StationPrefs SettingsDialog::prefs() const
{
    StationPrefs p;
    p.callsign = ragtime::protocol::normalCallsign(text(callsign_));
    p.locator = ragtime::globus::tidyLocator(text(locator_));
    p.host = text(host_);
    if (p.host.empty()) p.host = "127.0.0.1";
    p.kissPort = kissPort_->GetValue();
    p.commandPort = commandPort_->GetValue();
    p.receivedFolder = std::string(received_->GetPath().Strip(wxString::both).utf8_str());
    wxStringTokenizer words(autoAccept_->GetValue(), ", ;");
    while (words.HasMoreTokens())
    {
        std::string call = ragtime::protocol::normalCallsign(std::string(words.GetNextToken().utf8_str()));
        if (!call.empty()) p.autoAccept.push_back(call);
    }
    p.sealChat = sealChat_->GetValue();
    p.slowRepairMinutes = slowRepair_->GetValue();
    int floor = repairFloor_->GetSelection();
    p.repairFloor = floor > 0 && (size_t)floor < floorModes_.size() ? floorModes_[(size_t)floor] : std::string();
    p.repairCap = repairCap_->GetValue();
    p.repairCapMinutes = repairCapMinutes_->GetValue();
    p.readDial = readDial_->GetValue();
    p.rigHost = text(rigHost_);
    if (p.rigHost.empty()) p.rigHost = "127.0.0.1";
    p.rigPort = rigPort_->GetValue();
    p.usSegmentsOnly = p.readDial && usSegmentsOnly_->GetValue();
    return p;
}

void SettingsDialog::check()
{
    dataPort_->SetLabel(wxString::Format("Session data on %d", commandPort_->GetValue() + 1));
    repairCapMinutes_->Enable(repairCap_->GetValue());
    // The segment check needs the dial.
    rigHost_->Enable(readDial_->GetValue());
    rigPort_->Enable(readDial_->GetValue());
    usSegmentsOnly_->Enable(readDial_->GetValue());
    wxString problem;
    std::string call = text(callsign_);
    std::string locator = text(locator_);
    if (!call.empty() && ragtime::protocol::normalCallsign(call).empty())
        problem = "A callsign is letters, digits, / and -, at most 10 characters.";
    else if (!locator.empty() && ragtime::globus::tidyLocator(locator).empty())
        problem = "A locator is 4 or 6 characters, such as CN87 or CN87ux.";
    wxStringTokenizer words(autoAccept_->GetValue(), ", ;");
    while (problem.empty() && words.HasMoreTokens())
    {
        wxString word = words.GetNextToken();
        if (ragtime::protocol::normalCallsign(std::string(word.utf8_str())).empty())
            problem = "\"" + word + "\" isn't a callsign: list the stations separated by spaces.";
    }
    problem_->SetLabel(problem);
    if (wxWindow* ok = FindWindow(wxID_OK)) ok->Enable(problem.empty());
    Layout();
}

bool SettingsDialog::TransferDataFromWindow()
{
    return problem_->GetLabel().empty();
}
