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
// Three badges stand for states Mark has no name for (Extra): Not
// delivered (a session message), the seal in red, outlined, with a slash;
// Acknowledged (an @message whose station sent its Ack, not sealed), Held's
// double check alone, nearly the badge's size, like Awaiting's clock; and
// No ack (an @message sent, its Ack not back), Sealed's check alone, grey.
enum class Extra
{
    None,
    NotDelivered,
    Acknowledged,
    NoAck,
};

void drawSeal(wxGraphicsContext& gc, double size, Mark mark, bool dark, Extra extra = Extra::None)
{
    const bool slashed = extra == Extra::NotDelivered;
    const double u = size / 24.0;
    const double pi = std::acos(-1.0);
    Shade shade = shadeOf(mark, dark);
    if (slashed)
        shade = dark ? Shade{wxColour(0xff, 0x86, 0x78), wxColour(0x3d, 0x1a, 0x17)}
                     : Shade{wxColour(0xb4, 0x23, 0x18), wxColour(0xfb, 0xe4, 0xe1)};

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

    if (extra == Extra::Acknowledged)
    {
        // Held's two ticks, 1.65 times as big about their middle.
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        pen(2);
        line({{2.76, 12.6}, {6.72, 16.55}, {14.8, 7.65}});
        line({{12, 15.89}, {12.66, 16.55}, {20.74, 7.65}});
        return;
    }
    if (extra == Extra::NoAck)
    {
        // Sealed's tick, 1.9 times as big about its middle.
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        pen(2);
        line({{4.4, 12.39}, {9.53, 17.52}, {19.6, 6.88}});
        return;
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
        case Extra::Acknowledged:
            return "acknowledged";
        case Extra::NoAck:
            return "noack";
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

wxString acknowledgedBadgeHtml(const wxWindow* window)
{
    return badgeHtml(Mark::Held, Extra::Acknowledged, window);
}

wxString noAckBadgeHtml(const wxWindow* window)
{
    return badgeHtml(Mark::Awaiting, Extra::NoAck, window);
}
