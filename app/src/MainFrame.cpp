#include "MainFrame.h"

#include <optional>
#include <utility>

#include <wx/log.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GearshiftDialog.h"
#include "Thresholds.h"
#include "ragtime/Version.h"

namespace
{

const int ID_GEARSHIFT = wxID_HIGHEST + 1;

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

    // Until package E puts the width and Gear buttons in, the gearshift
    // opens from the Settings menu.
    auto* settings = new wxMenu;
    settings->Append(ID_GEARSHIFT, "&Gearshift...\tCtrl+G");
    auto* menus = new wxMenuBar;
    menus->Append(settings, "&Settings");
    SetMenuBar(menus);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { showGearshift(); }, ID_GEARSHIFT);

    CreateStatusBar(2);
    SetStatusText(wxString::FromUTF8("Ragtime ") + ragtime::versionString()
                  + wxString::FromUTF8(" · not connected"));
    loadGearshift();
    Centre();
}

void MainFrame::loadGearshift()
{
    const wxString path = thresholdsPath();
    ragtime::gearbox::SubmodeTable table;
    wxString error;
    if (!path.empty() && !loadThresholds(path, table, error))
        wxLogWarning("Gearshift: %s", error);
    gearshift_.setTable(std::move(table));
    showGear();
}

void MainFrame::showGearshift()
{
    GearshiftDialog dialog(this, gearshift_, [this]() { showGear(); });
    dialog.ShowModal();
}

void MainFrame::showGear()
{
    const std::optional<ragtime::gearbox::Gear> gear = gearshift_.current();
    SetStatusText(gear ? wxString::FromUTF8(ragtime::gearbox::gearLabel(*gear))
                       : wxString("No gears: load thresholds in Settings > Gearshift"),
                  1);
}
