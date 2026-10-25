#include "WatchDialog.h"

#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "Html.h"
#include "ragtime/protocol/Fields.h"

namespace
{

std::string callIn(const wxTextCtrl* entry)
{
    return ragtime::protocol::normalCallsign(std::string(entry->GetValue().Strip(wxString::both).utf8_str()));
}

} // namespace

WatchDialog::WatchDialog(wxWindow* parent, const std::vector<std::string>& calls)
    : wxDialog(parent, wxID_ANY, "Watch list")
{
    auto* intro = new wxStaticText(this, wxID_ANY,
                                   "A bell on each of these stations, heard before or not: a notification when "
                                   "one is heard on D2G after half an hour or more of quiet, or sends QRV.");
    intro->Wrap(FromDIP(360));

    entry_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    entry_->SetHint("KC2G");
    add_ = new wxButton(this, wxID_ADD, "Add");
    auto* addRow = new wxBoxSizer(wxHORIZONTAL);
    addRow->Add(entry_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    addRow->Add(add_, 0, wxALIGN_CENTER_VERTICAL);

    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(180)), 0, nullptr,
                          wxLB_SINGLE | wxLB_SORT);
    for (const std::string& call : calls) list_->Append(wxString::FromUTF8(call));
    remove_ = new wxButton(this, wxID_REMOVE, "Remove");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(addRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(remove_, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
    SetSizerAndFit(sizer);
    CentreOnParent();

    add_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add(); });
    entry_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { add(); });
    entry_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { showButtons(); });
    list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { showButtons(); });
    remove_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int at = list_->GetSelection();
        if (at != wxNOT_FOUND) list_->Delete((unsigned)at);
        showButtons();
    });
    showButtons();
    entry_->SetFocus();
}

void WatchDialog::add()
{
    std::string call = callIn(entry_);
    if (call.empty()) return;
    wxString item = wxString::FromUTF8(call);
    if (list_->FindString(item, true) == wxNOT_FOUND) list_->Append(item);
    list_->SetStringSelection(item);
    entry_->Clear();
    showButtons();
}

void WatchDialog::showButtons()
{
    add_->Enable(!callIn(entry_).empty());
    remove_->Enable(list_->GetSelection() != wxNOT_FOUND);
}

std::vector<std::string> WatchDialog::calls() const
{
    std::vector<std::string> out;
    for (unsigned i = 0; i < list_->GetCount(); i++) out.push_back(std::string(list_->GetString(i).utf8_str()));
    return out;
}
