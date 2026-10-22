#ifndef RAGTIME_GLOBUS_VIEW_H
#define RAGTIME_GLOBUS_VIEW_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <wx/bitmap.h>
#include <wx/timer.h>
#include <wx/window.h>

#include "ragtime/globus/Globe.h"
#include "ragtime/globus/Sun.h"

// The globe itself: a whole earth, lit from where the sun is overhead so
// the greyline shows, with the path to the picked station as a red great
// circle, our own square a navy dot and the other stations red dots.
//
// Given our square and a station's it rolls to show the path and eases to
// a stop, the path curling out from our dot to the station. Dragged it
// spins, and with only our own square to show it coasts on until the fluid
// stops it; held, it stops almost at once. The mouse wheel zooms in and
// out; a double click fits the path again. It is ported from Glissando's
// map ball, and the sums are in ragtime/globus/Globe.h and Sun.h.
class GlobusView : public wxWindow
{
public:
    explicit GlobusView(wxWindow* parent, wxWindowID id = wxID_ANY);

    // Our locator, and the picked station's, either possibly empty, with
    // whether the station's came from it this contact (an older one shows
    // dim). The globe rolls when they change; the first ones it is given,
    // it starts at. Until then it shows the North Atlantic.
    void setLocators(const std::string& home, const std::string& station, bool stationCurrent = true);

    // The squares of the other stations in the station list, each with
    // whether it is current: red dots, faint for an old square.
    void setStations(const std::vector<std::pair<std::string, bool>>& stations);

    // The moment the globe is lit for, in seconds since 1970 UTC.
    void setTime(std::int64_t unixSeconds);

    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnDoubleClick(wxMouseEvent& event);
    void OnMouseDown(wxMouseEvent& event);
    void OnMouseUp(wxMouseEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);

    bool showsPath() const { return haveHome_ && haveStation_; }

    // The view that shows what we have best: the path, or one end of it;
    // and the magnet that draws the globe there.
    ragtime::globus::View fitted() const;
    ragtime::globus::Vec3 magnet() const;
    void rollTo(const ragtime::globus::View& view);
    void wake();

    // Where the mouse is, in pixels right and up from the window's middle.
    void pointer(const wxMouseEvent& event, double& x, double& y) const;

    const wxBitmap& lighting(const ragtime::globus::View& view, double scale);

    ragtime::globus::Window window_;
    ragtime::globus::Zoom zoom_;
    ragtime::globus::Roller roller_;
    bool placed_ = false;           // the globe has been put at a grid square

    std::string home_;
    std::string station_;
    bool haveHome_ = false;
    bool haveStation_ = false;
    bool stationCurrent_ = true;
    ragtime::globus::LatLon homeAt_;
    ragtime::globus::LatLon stationAt_;
    std::vector<std::pair<std::string, bool>> stations_;
    std::vector<std::pair<ragtime::globus::LatLon, bool>> dots_;
    double pathDrawn_ = 1.0;        // how much of the path is drawn so far, 0 to 1

    ragtime::globus::LatLon sun_;

    // The light, kept until the view, the sun or the size changes.
    ragtime::globus::Overlay overlay_;
    wxBitmap lightingBitmap_;
    ragtime::globus::View lightingView_;
    ragtime::globus::LatLon lightingSun_;
    double lightingScale_ = 0.0;
    ragtime::globus::Window lightingWindow_;

    wxTimer timer_;
    long long lastTickMs_ = 0;
};

#endif
