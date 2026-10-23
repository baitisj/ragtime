#include "GlobusPanel.h"

#include <cstdint>
#include <ctime>
#include <string>

#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include "GlobusToggle.h"
#include "GlobusView.h"
#include "../Theme.h"
#include "ragtime/globus/Globe.h"
#include "ragtime/globus/Sun.h"

namespace
{

// The lighting follows the clock; a minute's turn of the earth is a
// quarter of a degree, so twice a minute is plenty.
constexpr int CLOCK_MILLISECONDS = 30000;

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text.c_str());
}

} // namespace

GlobusPanel::GlobusPanel(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id)
    , clock_(this)
{
    // The pane is a surface, like the tabs' pages beside it.
    followThemeBackground(this, wxSYS_COLOUR_WINDOW);
    toggle_ = new GlobusToggle(this);
    time_ = new wxStaticText(this, wxID_ANY, "");
    time_->SetToolTip("The time the globe is lit for, in UTC and your local time");
    view_ = new GlobusView(this);
    caption_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    caption_->SetFont(caption_->GetFont().Bold());
    details_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(toggle_, 0, wxALIGN_CENTER_VERTICAL);
    header->AddStretchSpacer();
    header->Add(time_, 0, wxALIGN_CENTER_VERTICAL);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(header, 0, wxEXPAND | wxALL, 6);
    // The globe stays square as the pane is resized, the captions right
    // under it.
    sizer->Add(view_, 1, wxSHAPED | wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, 6);
    sizer->Add(caption_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(details_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    sizer->AddStretchSpacer(1);
    SetSizer(sizer);

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { updateTime(); });
    clock_.Start(CLOCK_MILLISECONDS);
    updateTime();
    updateCaption();
}

void GlobusPanel::setHome(const std::string& locator)
{
    home_ = locator;
    view_->setLocators(home_, locator_, current_);
    updateCaption();
}

void GlobusPanel::setStation(const std::string& callsign, const std::string& locator, bool current,
                             const std::string& details)
{
    callsign_ = callsign;
    locator_ = locator;
    current_ = current;
    view_->setLocators(home_, locator_, current_);
    details_->SetLabel(utf8(details));
    updateCaption();
}

void GlobusPanel::setStations(const std::vector<std::pair<std::string, bool>>& stations)
{
    view_->setStations(stations);
}

void GlobusPanel::showGlobe(bool show)
{
    toggle_->setOn(show);
    time_->Show(show);
    view_->Show(show);
    caption_->Show(show);
    details_->Show(show);
    Layout();
}

bool GlobusPanel::globeShown() const
{
    return view_->IsShown();
}

void GlobusPanel::updateTime()
{
    std::time_t now = std::time(nullptr);
    const std::tm* localNow = std::localtime(&now);
    std::tm local = localNow ? *localNow : std::tm{};
    const std::tm* utcNow = std::gmtime(&now);
    std::tm utc = utcNow ? *utcNow : std::tm{};
    char zone[64] = "";
    if (localNow) std::strftime(zone, sizeof zone, "%Z", &local);
    int offset = localNow && utcNow ? ragtime::globus::utcOffsetSeconds(local, utc) : 0;
    time_->SetLabel(utf8(ragtime::globus::lightingTime((std::int64_t)now, offset, zone)));
    view_->setTime((std::int64_t)now);
    Layout();
}

void GlobusPanel::updateCaption()
{
    std::string text = ragtime::globus::caption(home_, callsign_, locator_, current_);
    if (ragtime::globus::tidyLocator(home_).empty() && callsign_.empty())
    {
        text = "Set your locator to see the paths";
    }
    caption_->SetLabel(utf8(text));
    caption_->SetForegroundColour(current_ || callsign_.empty() ? wxNullColour
                                                                 : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    Layout();
}
