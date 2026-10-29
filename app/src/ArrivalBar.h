#ifndef RAGTIME_ARRIVAL_BAR_H
#define RAGTIME_ARRIVAL_BAR_H

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/control.h>

// Under the picture dialog's preview (Jeff, 2026-10-29): a progress bar of
// how much of the picture has come as the preview plays, the bursts' ends
// marked across it, the time it takes on an axis over it and a scale of
// the picture's size under it. Hovering over it pauses the preview and
// lets the operator drag through it; away from it, the preview plays on.
// Over either axis's largest figure it lights the settings that set them.
// When the gear is too slow, the bar bursts red out of its right end.
class ArrivalBar : public wxControl
{
public:
    ArrivalBar(wxWindow* parent, wxWindowID id = wxID_ANY);

    // The whole picture's bytes, where each burst ends and how long it
    // all takes (no bytes: no bar).
    void setWhole(uint64_t whole, const std::vector<uint64_t>& burstEnds, double seconds);
    // How much has come.
    void setDone(uint64_t done);
    // Not even a small picture fits the airtime: the burst bar, under a
    // time axis up to that airtime.
    void setTooSlow(double seconds);

    // The pointer came over the bar (true) or went away (false).
    std::function<void(bool over)> onHover;
    // Pressed or dragged to how much has come at that point.
    std::function<void(uint64_t bytes)> onSeek;
    // The pointer came over an axis's largest figure (true) or left it.
    std::function<void(bool lit)> onLimits;

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void OnMouse(wxMouseEvent& event);
    void hover(bool over);
    void light(bool lit);
    void seekTo(int x);

    uint64_t whole_ = 0;
    uint64_t done_ = 0;
    std::vector<uint64_t> ends_;
    double seconds_ = 0.0;
    bool slow_ = false;
    bool over_ = false;     // the pointer over the bar, or dragging it
    bool lit_ = false;      // the pointer over a largest figure
    wxRect timeFigure_;     // where the axes' largest figures are drawn
    wxRect sizeFigure_;
};

#endif
