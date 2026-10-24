#include "SealBadge.h"

#include <cmath>
#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

#include <wx/bitmap.h>
#include <wx/bmpbndl.h>
#include <wx/brush.h>
#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/geometry.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/window.h>

#include "DrawnIcon.h"
#include "Html.h"

using Mark = ragtime::broadcast::Seals::Mark;

namespace
{

// Each icon's ink and tint, light and dark (Jeff, 2026-10-23: the light
// shades get lost on a dark list; the dark ones are Html.h's dark text
// colours).
struct Shade
{
    wxColour ink, tint;
};

Shade shadeOf(Mark mark, bool dark)
{
    switch (mark)
    {
        case Mark::Awaiting:
            return dark ? Shade{wxColour(0xa9, 0xb0, 0xbb), wxColour(0x2c, 0x30, 0x36)}
                        : Shade{wxColour(0x5b, 0x63, 0x70), wxColour(0xf1, 0xf2, 0xf4)};
        case Mark::Repairing:
            return dark ? Shade{wxColour(0xff, 0xcf, 0x5c), wxColour(0x3d, 0x2c, 0x10)}
                        : Shade{wxColour(0x8a, 0x4b, 0x00), wxColour(0xfb, 0xee, 0xdd)};
        case Mark::Mismatch:
            return dark ? Shade{*wxWHITE, wxColour(0xe5, 0x48, 0x3a)} : Shade{*wxWHITE, wxColour(0xb4, 0x23, 0x18)};
        case Mark::Sealed:
        case Mark::Held:
        case Mark::None:
            break;
    }
    return dark ? Shade{wxColour(0x6c, 0xd4, 0x86), wxColour(0x1d, 0x3a, 0x26)}
                : Shade{wxColour(0x1e, 0x6b, 0x34), wxColour(0xe3, 0xf1, 0xe6)};
}

const char* nameOf(Mark mark)
{
    switch (mark)
    {
        case Mark::Awaiting:
            return "awaiting";
        case Mark::Sealed:
            return "sealed";
        case Mark::Repairing:
            return "repairing";
        case Mark::Held:
            return "held";
        case Mark::Mismatch:
            return "mismatch";
        case Mark::None:
            break;
    }
    return "";
}

// The icons of docs/images/seals/, drawn on their 24-unit grid: a wax-seal
// rosette (48 points at radius 9.4 + 0.9 cos 12θ around (12, 12), stroked
// 1.5, dashed 2.2 on and 1.6 off while waiting), and a glyph stroked 2.
void drawSeal(wxGraphicsContext& gc, double size, Mark mark, bool dark)
{
    const double u = size / 24.0;
    const double pi = std::acos(-1.0);
    Shade shade = shadeOf(mark, dark);
    // A solid seal means look at this: its edge is its fill.
    bool solid = mark == Mark::Mismatch;
    bool dashed = mark == Mark::Awaiting || mark == Mark::Repairing;

    std::vector<wxPoint2DDouble> edge;
    for (int k = 0; k <= 48; k++)
    {
        double t = -pi / 2 + k * 2 * pi / 48;
        double r = 9.4 + 0.9 * std::cos(12 * t);
        edge.emplace_back((12 + r * std::cos(t)) * u, (12 + r * std::sin(t)) * u);
    }
    wxGraphicsPath rosette = gc.CreatePath();
    rosette.MoveToPoint(edge[0]);
    for (size_t i = 1; i < edge.size(); i++) rosette.AddLineToPoint(edge[i]);
    rosette.CloseSubpath();
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(shade.tint));
    gc.FillPath(rosette);

    wxColour rim = solid ? shade.tint : shade.ink;
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(rim, 1.5 * u).Cap(wxCAP_BUTT).Join(wxJOIN_ROUND)));
    if (!dashed)
    {
        gc.StrokePath(rosette);
    }
    else
    {
        // Dashes walked along the edge, the same in every backend.
        const double on = 2.2 * u, off = 1.6 * u;
        double left = on;
        bool drawing = true;
        wxGraphicsPath dashes = gc.CreatePath();
        dashes.MoveToPoint(edge[0]);
        for (size_t i = 1; i < edge.size(); i++)
        {
            wxPoint2DDouble a = edge[i - 1], b = edge[i];
            double length = a.GetDistance(b), at = 0;
            while (length - at > left)
            {
                at += left;
                wxPoint2DDouble p = a + (b - a) * (at / length);
                if (drawing)
                    dashes.AddLineToPoint(p);
                else
                    dashes.MoveToPoint(p);
                drawing = !drawing;
                left = drawing ? on : off;
            }
            left -= length - at;
            if (drawing)
                dashes.AddLineToPoint(b);
            else
                dashes.MoveToPoint(b);
        }
        gc.StrokePath(dashes);
    }

    auto pen = [&](double width) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(shade.ink, width * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    };
    auto line = [&](std::initializer_list<std::pair<double, double>> points) {
        wxGraphicsPath path = gc.CreatePath();
        bool first = true;
        for (const auto& p : points)
        {
            if (first)
                path.MoveToPoint(p.first * u, p.second * u);
            else
                path.AddLineToPoint(p.first * u, p.second * u);
            first = false;
        }
        gc.StrokePath(path);
    };
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    pen(2);
    switch (mark)
    {
        case Mark::Sealed:
            line({{8, 12.3}, {10.7, 15}, {16, 9.4}});
            break;
        case Mark::Held:
            line({{6.3, 12.4}, {8.7, 14.8}, {13.6, 9.4}});
            line({{11.9, 14.4}, {12.3, 14.8}, {17.2, 9.4}});
            break;
        case Mark::Awaiting:
            gc.DrawEllipse(7.7 * u, 7.7 * u, 8.6 * u, 8.6 * u);
            pen(1.7);
            line({{12, 9.6}, {12, 12.2}, {13.8, 13.4}});
            break;
        case Mark::Repairing:
        {
            // An arc of radius 4 nearly round, and its arrowhead.
            wxGraphicsPath arc = gc.CreatePath();
            arc.AddArc(11.89 * u, 12.11 * u, 4 * u, 21.9 * pi / 180, 321.1 * pi / 180, true);
            gc.StrokePath(arc);
            line({{15.6, 7}, {15.6, 9.9}, {12.7, 9.9}});
            break;
        }
        case Mark::Mismatch:
            line({{9.3, 9.3}, {14.7, 14.7}});
            line({{14.7, 9.3}, {9.3, 14.7}});
            break;
        case Mark::None:
            break;
    }
}

} // namespace

wxString sealBadgeHtml(Mark mark, const wxWindow* window)
{
    if (mark == Mark::None || window == nullptr) return wxString();
    bool dark = darkSurface();
    int px = window->FromDIP(SEAL_BADGE_PX);
    wxString name = wxString::Format("ragtime-seal-%s-%s-%d.png", nameOf(mark), dark ? "dark" : "light", px);

    // Each icon is drawn once a theme and size, into wxHTML's memory files.
    static std::set<wxString> made;
    if (made.count(name) == 0)
    {
        static bool handler = false;
        if (!handler)
        {
            wxFileSystem::AddHandler(new wxMemoryFSHandler);
            handler = true;
        }
        if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
        wxBitmapBundle icon = drawnIcon([mark, dark](wxGraphicsContext& gc, double size) { drawSeal(gc, size, mark, dark); },
                                        SEAL_BADGE_PX);
        wxBitmap bitmap = icon.GetBitmap(wxSize(px, px));
        if (!bitmap.IsOk()) return wxString();
        wxMemoryFSHandler::AddFile(name, bitmap.ConvertToImage(), wxBITMAP_TYPE_PNG);
        made.insert(name);
    }
    wxString side = wxString::Format("%d", px);
    return "<img src=\"memory:" + name + "\" width=\"" + side + "\" height=\"" + side + "\">";
}
