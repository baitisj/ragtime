#ifndef RAGTIME_GLOBUS_PANEL_H
#define RAGTIME_GLOBUS_PANEL_H

#include <string>
#include <utility>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

class GlobusToggle;
class GlobusView;
class wxStaticText;

// The Globus pane (docs/MOCKUPS.md): a header with the Globus toggle and
// the time the globe is lit for, "22:36 UTC / 15:36 PDT"; the globe; and
// under it the caption, "W1AW · FN31 · 3,951 km · 81°", then the station's
// details. The toggle sends wxEVT_TOGGLEBUTTON up to the window that holds
// the pane, which shows or hides it (package E).
class GlobusPanel : public wxPanel
{
public:
    explicit GlobusPanel(wxWindow* parent, wxWindowID id = wxID_ANY);

    // Our own locator; empty until the operator sets one.
    void setHome(const std::string& locator);

    // The station picked in the station list (empty for none), its locator
    // if known and whether that came from it this contact, and a line of
    // details for under the caption.
    void setStation(const std::string& callsign, const std::string& locator, bool current = true,
                    const std::string& details = "");

    // The squares of the stations in the station list, as red dots.
    void setStations(const std::vector<std::pair<std::string, bool>>& stations);

    // Shows the globe and its captions, or only the header, and lights the
    // toggle to match.
    void showGlobe(bool show);
    bool globeShown() const;

    GlobusToggle* toggle() const { return toggle_; }

private:
    void updateTime();
    void updateCaption();

    GlobusToggle* toggle_ = nullptr;
    wxStaticText* time_ = nullptr;
    GlobusView* view_ = nullptr;
    wxStaticText* caption_ = nullptr;
    wxStaticText* details_ = nullptr;
    wxTimer clock_;

    std::string home_;
    std::string callsign_;
    std::string locator_;
    bool current_ = true;
};

#endif
