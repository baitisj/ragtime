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
// 1.5) and a glyph stroked 2. The two still waiting, Awaiting and
// Repairing, have no rosette: their glyph alone, nearly the badge's size
// (Jeff, 2026-10-24: no dotted rings, bigger centres).
// Not delivered (a session message) has no Mark: the seal in red,
// outlined, with a slash. Nor have the StatusBadge states; Extra names
// them all.
enum class Extra
{
    None,
    NotDelivered,
    Queued,
    Sending,
    Held,
    Sent,
    Acknowledged,
    NotSent,
    Cancelled,
};

Extra extraOf(StatusBadge badge)
{
    switch (badge)
    {
        case StatusBadge::Queued:
            return Extra::Queued;
        case StatusBadge::Sending:
            return Extra::Sending;
        case StatusBadge::Held:
            return Extra::Held;
        case StatusBadge::Sent:
            return Extra::Sent;
        case StatusBadge::Acknowledged:
            return Extra::Acknowledged;
        case StatusBadge::NotSent:
            return Extra::NotSent;
        case StatusBadge::Cancelled:
            break;
    }
    return Extra::Cancelled;
}

Shade redShade(bool dark)
{
    return dark ? Shade{wxColour(0xff, 0x86, 0x78), wxColour(0x3d, 0x1a, 0x17)}
                : Shade{wxColour(0xb4, 0x23, 0x18), wxColour(0xfb, 0xe4, 0xe1)};
}

Shade shadeOf(Mark mark, Extra extra, bool dark)
{
    switch (extra)
    {
        case Extra::NotDelivered:
        case Extra::NotSent:
            return redShade(dark);
        case Extra::Sending:
            return dark ? Shade{wxColour(0x7f, 0xb0, 0xff), wxColour(0x1b, 0x2a, 0x44)}
                        : Shade{wxColour(0x1f, 0x5f, 0xbf), wxColour(0xe4, 0xec, 0xf8)};
        case Extra::Queued:
        case Extra::Held:
        case Extra::Sent:
        case Extra::Cancelled:
            return shadeOf(Mark::Awaiting, dark);
        case Extra::Acknowledged:
            return shadeOf(Mark::Held, dark);
        case Extra::None:
            break;
    }
    return shadeOf(mark, dark);
}

void drawSeal(wxGraphicsContext& gc, double size, Mark mark, bool dark, Extra extra = Extra::None)
{
    const bool slashed = extra == Extra::NotDelivered;
    const double u = size / 24.0;
    const double pi = std::acos(-1.0);
    Shade shade = shadeOf(mark, extra, dark);

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
    // An arc around (cx, cy) from a0 to a1 degrees (clockwise on screen as
    // they grow), walked in short lines, the same in every backend.
    auto arc = [&](double cx, double cy, double r, double a0, double a1) {
        wxGraphicsPath path = gc.CreatePath();
        const int steps = 24;
        for (int k = 0; k <= steps; k++)
        {
            double a = (a0 + (a1 - a0) * k / steps) * pi / 180;
            double x = (cx + r * std::cos(a)) * u, y = (cy + r * std::sin(a)) * u;
            if (k == 0)
                path.MoveToPoint(x, y);
            else
                path.AddLineToPoint(x, y);
        }
        gc.StrokePath(path);
    };
    // A dot of radius r, filled in the ink.
    auto dot = [&](double cx, double cy, double r) {
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(shade.ink));
        gc.DrawEllipse((cx - r) * u, (cy - r) * u, 2 * r * u, 2 * r * u);
    };
    // A face of radius 8.2 around the middle, tinted.
    auto face = [&]() {
        gc.SetBrush(wxBrush(shade.tint));
        pen(2);
        gc.DrawEllipse(3.8 * u, 3.8 * u, 16.4 * u, 16.4 * u);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
    };

    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    pen(2);
    switch (extra)
    {
        case Extra::Queued:
        {
            // An hourglass, its sand run into the foot.
            line({{6.5, 3.5}, {17.5, 3.5}});
            line({{6.5, 20.5}, {17.5, 20.5}});
            wxGraphicsPath glass = gc.CreatePath();
            glass.MoveToPoint(8 * u, 3.5 * u);
            glass.AddCurveToPoint(8 * u, 8 * u, 16 * u, 8 * u, 16 * u, 12 * u);
            glass.AddCurveToPoint(16 * u, 16 * u, 8 * u, 16 * u, 8 * u, 20.5 * u);
            glass.MoveToPoint(16 * u, 3.5 * u);
            glass.AddCurveToPoint(16 * u, 8 * u, 8 * u, 8 * u, 8 * u, 12 * u);
            glass.AddCurveToPoint(8 * u, 16 * u, 16 * u, 16 * u, 16 * u, 20.5 * u);
            gc.StrokePath(glass);
            wxGraphicsPath sand = gc.CreatePath();
            sand.MoveToPoint(9.6 * u, 19.2 * u);
            sand.AddLineToPoint(12 * u, 16.8 * u);
            sand.AddLineToPoint(14.4 * u, 19.2 * u);
            sand.CloseSubpath();
            gc.SetBrush(wxBrush(shade.ink));
            gc.FillPath(sand);
            return;
        }
        case Extra::Sending:
            // A radio mast, and its waves either side.
            line({{12, 13}, {12, 21.5}});
            arc(12, 10.5, 5.2, 135, 225);
            arc(12, 10.5, 5.2, -45, 45);
            arc(12, 10.5, 9.6, 135, 225);
            arc(12, 10.5, 9.6, -45, 45);
            dot(12, 10.5, 1.6);
            return;
        case Extra::Held:
            // A pause sign on a tinted face.
            face();
            line({{9.6, 8.2}, {9.6, 15.8}});
            line({{14.4, 8.2}, {14.4, 15.8}});
            return;
        case Extra::Sent:
            // Sealed's tick, 1.9 times as big about its middle.
            line({{4.4, 12.39}, {9.53, 17.52}, {19.6, 6.88}});
            return;
        case Extra::Acknowledged:
            // Held's two ticks, 1.65 times as big about their middle.
            line({{2.76, 12.6}, {6.72, 16.55}, {14.8, 7.65}});
            line({{12, 15.89}, {12.66, 16.55}, {20.74, 7.65}});
            return;
        case Extra::NotSent:
            // "!" on a tinted face.
            face();
            line({{12, 7.4}, {12, 12.8}});
            dot(12, 16.4, 1.25);
            return;
        case Extra::Cancelled:
            line({{7.2, 7.2}, {16.8, 16.8}});
            line({{16.8, 7.2}, {7.2, 16.8}});
            return;
        case Extra::None:
        case Extra::NotDelivered:
            break;
    }

    if (!slashed && mark == Mark::Awaiting)
    {
        // A clock face of radius 8.2, tinted, and its hands.
        gc.SetBrush(wxBrush(shade.tint));
        pen(2);
        gc.DrawEllipse(3.8 * u, 3.8 * u, 16.4 * u, 16.4 * u);
        line({{12, 7.4}, {12, 12.4}, {15.4, 14.7}});
        return;
    }
    if (!slashed && mark == Mark::Repairing)
    {
        // An arc of radius 7.6 nearly round, and its arrowhead.
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        pen(2);
        wxGraphicsPath arc = gc.CreatePath();
        arc.AddArc(11.79 * u, 12.21 * u, 7.6 * u, 21.9 * pi / 180, 321.1 * pi / 180, true);
        gc.StrokePath(arc);
        line({{18.8, 2.5}, {18.8, 8}, {13.3, 8}});
        return;
    }

    // A solid seal means look at this: its edge is its fill.
    bool solid = mark == Mark::Mismatch;

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
    gc.StrokePath(rosette);

    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    pen(2);
    if (slashed)
    {
        line({{8.6, 15.4}, {15.4, 8.6}});
        return;
    }
    switch (mark)
    {
        case Mark::Sealed:
            line({{8, 12.3}, {10.7, 15}, {16, 9.4}});
            break;
        case Mark::Held:
            line({{6.3, 12.4}, {8.7, 14.8}, {13.6, 9.4}});
            line({{11.9, 14.4}, {12.3, 14.8}, {17.2, 9.4}});
            break;
        case Mark::Mismatch:
            line({{9.3, 9.3}, {14.7, 14.7}});
            line({{14.7, 9.3}, {9.3, 14.7}});
            break;
        case Mark::Awaiting:
        case Mark::Repairing:
        case Mark::None:
            break;
    }
}

} // namespace

namespace
{

const char* nameOf(Mark mark, Extra extra)
{
    switch (extra)
    {
        case Extra::NotDelivered:
            return "undelivered";
        case Extra::Queued:
            return "queued";
        case Extra::Sending:
            return "sending";
        case Extra::Held:
            return "on-hold";
        case Extra::Sent:
            return "sent";
        case Extra::Acknowledged:
            return "acknowledged";
        case Extra::NotSent:
            return "not-sent";
        case Extra::Cancelled:
            return "cancelled";
        case Extra::None:
            break;
    }
    return nameOf(mark);
}

wxString badgeHtml(Mark mark, Extra extra, const wxWindow* window)
{
    if ((mark == Mark::None && extra == Extra::None) || window == nullptr) return wxString();
    bool dark = darkSurface();
    int px = window->FromDIP(SEAL_BADGE_PX);
    wxString name = wxString::Format("ragtime-seal-%s-%s-%d.png", nameOf(mark, extra),
                                     dark ? "dark" : "light", px);

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
        wxBitmapBundle icon = drawnIcon(
            [mark, dark, extra](wxGraphicsContext& gc, double size) { drawSeal(gc, size, mark, dark, extra); },
            SEAL_BADGE_PX);
        wxBitmap bitmap = icon.GetBitmap(wxSize(px, px));
        if (!bitmap.IsOk()) return wxString();
        wxMemoryFSHandler::AddFile(name, bitmap.ConvertToImage(), wxBITMAP_TYPE_PNG);
        made.insert(name);
    }
    wxString side = wxString::Format("%d", px);
    return "<img src=\"memory:" + name + "\" width=\"" + side + "\" height=\"" + side + "\">";
}

} // namespace

wxString sealBadgeHtml(Mark mark, const wxWindow* window)
{
    return badgeHtml(mark, Extra::None, window);
}

wxString deliveredBadgeHtml(const wxWindow* window)
{
    return badgeHtml(Mark::Held, Extra::None, window);
}

wxString notDeliveredBadgeHtml(const wxWindow* window)
{
    return badgeHtml(Mark::None, Extra::NotDelivered, window);
}

wxString statusBadgeHtml(StatusBadge badge, const wxWindow* window)
{
    return badgeHtml(Mark::None, extraOf(badge), window);
}
