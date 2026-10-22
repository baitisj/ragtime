#include "SettingsDialog.h"

#include <string>

#include <wx/button.h>
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

    problem_ = new wxStaticText(this, wxID_ANY, "");
    problem_->SetForegroundColour(wxColour(0xb4, 0x23, 0x18));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(station, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(problem_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
    SetSizer(sizer);
    // Room for the longest problem line, so the dialog never jumps.
    problem_->SetMinSize(wxSize(FromDIP(380), problem_->GetCharHeight()));
    Fit();

    auto changed = [this](wxCommandEvent&) { check(); };
    callsign_->Bind(wxEVT_TEXT, changed);
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
    problem_->SetLabel(problem);
    if (wxWindow* ok = FindWindow(wxID_OK)) ok->Enable(problem.empty());
    Layout();
}

bool SettingsDialog::TransferDataFromWindow()
{
    return problem_->GetLabel().empty();
}
