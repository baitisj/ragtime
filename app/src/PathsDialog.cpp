#include "PathsDialog.h"

#include <string>

#include <wx/button.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "FilesPage.h"
#include "Html.h"
#include "PathStripView.h"

PathsDialog::PathsDialog(wxWindow* parent, const ragtime::radio::PathStrip& strip, uint64_t nowMs,
                         const std::string& logPath)
    : wxDialog(parent, wxID_ANY, wxString::FromUTF8("Paths heard from " + strip.call))
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(
        this, wxID_ANY,
        wxString::Format("The hours (UTC) %s was heard in the last %u days, by band: the darker, the more days.",
                         wxString::FromUTF8(strip.call), (unsigned)strip.days));
    intro->Wrap(FromDIP(460));
    sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    auto* view = new PathStripView(this, 16);
    view->setStrip(strip, nowMs);
    sizer->Add(view, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    sizer->AddSpacer(8);
    for (const ragtime::radio::BandRow& row : strip.bands)
    {
        auto* line = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(ragtime::radio::rowSummary(row)));
        sizer->AddSpacer(4);
        sizer->Add(line, 0, wxLEFT | wxRIGHT, 12);
    }
    if (strip.bands.empty())
    {
        auto* none = new wxStaticText(this, wxID_ANY, "Nothing yet: each burst heard from it adds a line.");
        none->SetForegroundColour(secondaryText());
        sizer->Add(none, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }
    if (strip.bands.size() == 1 && strip.bands[0].band.empty())
    {
        auto* hint = new wxStaticText(this, wxID_ANY,
                                      "Bands show once Ragtime reads the dial: Settings > Station, Radio.");
        hint->SetForegroundColour(secondaryText());
        sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    }

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* open = new wxButton(this, wxID_ANY, "Open the path log");
    open->SetToolTip(wxString::FromUTF8(logPath));
    open->Enable(wxFileName::FileExists(wxString::FromUTF8(logPath)));
    open->Bind(wxEVT_BUTTON, [logPath](wxCommandEvent&) { openWithDesktop(logPath); });
    buttons->Add(open, 0);
    buttons->AddStretchSpacer();
    auto* close = new wxButton(this, wxID_CLOSE, "Close");
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });
    buttons->Add(close, 0);
    SetEscapeId(wxID_CLOSE);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 12);
    SetSizerAndFit(sizer);
    CentreOnParent();
}
