#ifndef RAGTIME_EAR_SCOPE_H
#define RAGTIME_EAR_SCOPE_H

#include <deque>
#include <optional>
#include <vector>

#include <wx/image.h>
#include <wx/panel.h>

#include "ragtime/ear/Edges.h"

// The Ear's picture (docs/STAGE1.md, "The Ear"): the averaged spectrum on
// top and the waterfall under it, newest row at the top, both from 0 to
// ear::Spectrum::TOP_HZ across. Data2G's passbands are shaded green (the
// one in use darker), and the frequency under the mouse is marked. Rows
// heard while the rig transmits are greyed: its receive audio is muted
// then.
//
// The filter's edges as the noise shows them (ear::EdgeFinder) are drawn
// in orange, with the -6 dB line between them, and the width the rig says
// as a dashed purple bracket over the top, centred where the noise puts
// the passband (or on Data2G's centre until it does).
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
    void setNarrow(bool narrow);
    // The edges the noise shows, if any.
    void setEdges(const std::optional<ragtime::ear::Edges>& edges);
    // The rig's filter width, in Hz; 0 when it hasn't said.
    void setRigWidth(int widthHz);

private:
    void paint();
    void scroll(const std::vector<float>& row, bool transmitting);
    void layoutAreas(int width, int height);

    static constexpr int HISTORY = 600;     // waterfall rows kept

    wxImage waterfall_;
    std::deque<std::vector<float>> recent_;     // for the averaged spectrum
    std::vector<float> average_;
    double floor_ = -100.0;
    bool haveFloor_ = false;
    double rowsPerSecond_ = 10.0;
    bool narrow_ = false;
    int rows_ = 0;
    int mouseX_ = -1;
    std::optional<ragtime::ear::Edges> edges_;
    int rigWidth_ = 0;

    // The areas, in pixels, worked out at each paint.
    wxRect plot_;       // the spectrum
    wxRect fall_;       // the waterfall
    int left_ = 0;      // the axis labels' margin
};

#endif
