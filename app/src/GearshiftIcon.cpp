#include "GearshiftIcon.h"

#include <cmath>

#include <wx/brush.h>
#include <wx/graphics.h>

#include "DrawnIcon.h"

namespace
{

constexpr double PI = 3.14159265358979323846;

} // namespace

void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid.
    const double u = size / 24.0;
    auto pen = [&](double width) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, width * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    };
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    // A dial from lower left round to lower right, ticks, and the needle
    // well up the scale.
    const double cx = 12 * u, cy = 13.5 * u, r = 10.5 * u;
    pen(2.0);
    wxGraphicsPath arc = gc.CreatePath();
    arc.AddArc(cx, cy, r, PI * 5 / 6, PI / 6, true);
    gc.StrokePath(arc);
    pen(1.6);
    for (int i = 0; i < 5; i++)
    {
        const double a = PI * 5 / 6 + i * (PI * 4 / 3) / 4;
        gc.StrokeLine(cx + std::cos(a) * r * 0.62, cy + std::sin(a) * r * 0.62,
                      cx + std::cos(a) * r * 0.80, cy + std::sin(a) * r * 0.80);
    }
    pen(2.2);
    const double needle = -PI / 4;  // up and to the right
    gc.StrokeLine(cx, cy, cx + std::cos(needle) * r * 0.78, cy + std::sin(needle) * r * 0.78);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawEllipse(cx - 2.2 * u, cy - 2.2 * u, 4.4 * u, 4.4 * u);
}

wxBitmapBundle gearshiftIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawGearshiftIcon(gc, size, ink); }, sizeDip);
}
