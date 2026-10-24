#include "SettingsDialog.h"

#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
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

SettingsDialog::SettingsDialog(wxWindow* parent, const StationPrefs& prefs)
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

    problem_ = new wxStaticText(this, wxID_ANY, "");
    problem_->SetForegroundColour(badText());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(station, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(files, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(broadcast, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
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
    return p;
}

void SettingsDialog::check()
{
    dataPort_->SetLabel(wxString::Format("Session data on %d", commandPort_->GetValue() + 1));
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
