#include "MainFrame.h"

#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "ragtime/Version.h"

namespace
{

wxPanel* placeholderPage(wxWindow* parent, const wxString& text)
{
    auto* page = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(new wxStaticText(page, wxID_ANY, text), 0, wxALIGN_CENTER_HORIZONTAL);
    sizer->AddStretchSpacer();
    page->SetSizer(sizer);
    return page;
}

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Ragtime", wxDefaultPosition, wxSize(1280, 720))
{
    // Every dialog and the main window must fit Jeff's 1366x768 screen.
    SetMinSize(wxSize(960, 600));

    tabs_ = new wxNotebook(this, wxID_ANY);
    // Tab order is fixed: Broadcast, Session, Files (CLAUDE.md).
    tabs_->AddPage(placeholderPage(tabs_, "Broadcast mode"), "Broadcast", true);
    tabs_->AddPage(placeholderPage(tabs_, "No session"), wxString::FromUTF8("Session · none"));
    tabs_->AddPage(placeholderPage(tabs_, "No files yet"), "Files");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(tabs_, 1, wxEXPAND);
    SetSizer(sizer);

    CreateStatusBar();
    SetStatusText(wxString::FromUTF8("Ragtime ") + ragtime::versionString()
                  + wxString::FromUTF8(" · not connected"));
    Centre();
}
