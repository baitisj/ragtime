#ifndef RAGTIME_EAR_SCOPE_H
#define RAGTIME_EAR_SCOPE_H

#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include <wx/image.h>
#include <wx/panel.h>

#include "ragtime/ear/Edges.h"
#include "ragtime/ear/FilterControl.h"
#include "ragtime/ear/Spectrum.h"

// The Ear's picture (docs/STAGE1.md, "The Ear"): the averaged spectrum on
// top and the waterfall under it, newest row at the top, both from 0 to
// the span across (setTopHz: 4 kHz, or as ear::AutoSpan picks). Data2G's passbands are shaded green (the
// one in use darker), and the frequency under the mouse is marked. Rows
// heard while the rig transmits are greyed: its receive audio is muted
// then.
//
// The filter's edges as the noise shows them (ear::EdgeFinder) are drawn
// in orange, with the -6 dB line between them, and the width the rig says
// as a dashed purple bracket over the top, centred where the noise puts
// the passband (or on Data2G's centre until it does).
//
// When the Ear may set the rig's filter, the edges get handles: drag one
// edge, or the band between them to shift the passband (until Calibrate
// has measured the knobs, both edges move together about the centre).
// Letting go hands the passband wanted to the owner, which draws it back
// as a dashed line until the change is kept or undone, with the part of
// Data2G's passband it would cut in red.
//
// The colours follow the noise: the floor is a slow average of each
// row's quieter part, and the waterfall runs from just under it to 50 dB
// over it, so a quiet band and a noisy one both show.
class EarScope : public wxPanel
{
public:
    explicit EarScope(wxWindow* parent);

    void addRows(const std::vector<std::vector<float>>& rows, bool transmitting);
    void clear();
    void setRowsPerSecond(double rowsPerSecond);
    // A new span: the waterfall so far is stretched or squeezed to it.
    void setTopHz(double topHz);
    void setNarrow(bool narrow);
    // The edges the noise shows, if any.
    void setEdges(const std::optional<ragtime::ear::Edges>& edges);
    // The rig's filter width, in Hz; 0 when it hasn't said.
    void setRigWidth(int widthHz);
    // The handles: where they sit, or none to hide them. together: both
    // edges move about the centre.
    void setHandles(const std::optional<ragtime::ear::Span>& at, bool together);
    void onDrag(std::function<void(const ragtime::ear::Span& from, const ragtime::ear::Span& to)> done)
    {
        dragged_ = std::move(done);
    }
    // The passband wanted, drawn dashed, or none.
    void setWanted(const std::optional<ragtime::ear::Span>& wanted);

private:
    void paint();
    void scroll(const std::vector<float>& row, bool transmitting);
    void layoutAreas(int width, int height);
    enum class Grip
    {
        None,
        Low,
        High,
        Shift,
    };
    Grip gripAt(int x, int y) const;
    double hzAt(int x) const;
    void mouseDown(wxMouseEvent& e);
    void mouseMove(wxMouseEvent& e);
    void mouseUp(wxMouseEvent& e);

    static constexpr int HISTORY = 600;     // waterfall rows kept

    wxImage waterfall_;
    std::deque<std::vector<float>> recent_;     // for the averaged spectrum
    std::vector<float> average_;
    double floor_ = -100.0;
    bool haveFloor_ = false;
    double rowsPerSecond_ = 10.0;
    double topHz_ = ragtime::ear::Spectrum::DEFAULT_TOP_HZ;
    bool narrow_ = false;
    int rows_ = 0;
    int mouseX_ = -1;
    std::optional<ragtime::ear::Edges> edges_;
    int rigWidth_ = 0;
    std::optional<ragtime::ear::Span> handles_;
    bool together_ = false;
    std::optional<ragtime::ear::Span> wanted_;
    std::function<void(const ragtime::ear::Span&, const ragtime::ear::Span&)> dragged_;
    Grip grip_ = Grip::None;        // being dragged
    double gripHz_ = 0;             // where the drag began
    ragtime::ear::Span dragFrom_;
    std::optional<ragtime::ear::Span> drag_;

    // The areas, in pixels, worked out at each paint.
    wxRect plot_;       // the spectrum
    wxRect fall_;       // the waterfall
    int left_ = 0;      // the axis labels' margin
};

#endif
