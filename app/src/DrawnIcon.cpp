#include "DrawnIcon.h"

#include <cmath>
#include <memory>
#include <set>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/window.h>

wxBitmapBundle drawnIcon(const IconDrawing& draw, int sizeDip)
{
    wxVector<wxBitmap> bitmaps;
    for (int scale : {1, 2})
    {
        const int px = sizeDip * scale;
        wxImage image(px, px);
        image.InitAlpha();
        unsigned char* alpha = image.GetAlpha();
        for (int i = 0; i < px * px; i++) alpha[i] = 0;
        {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
            if (gc)
            {
                gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
                draw(*gc, px);
            }
        }
        bitmaps.push_back(wxBitmap(image));
    }
    return wxBitmapBundle::FromBitmaps(bitmaps);
}

void drawFloppyIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid.
    const double u = size / 24.0;
    auto pen = [&](double width) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, width * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    };
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    // The case, its top right corner cut off.
    pen(1.8);
    wxGraphicsPath body = gc.CreatePath();
    body.MoveToPoint(3.5 * u, 3 * u);
    body.AddLineToPoint(17.5 * u, 3 * u);
    body.AddLineToPoint(21 * u, 6.5 * u);
    body.AddLineToPoint(21 * u, 20.5 * u);
    body.AddLineToPoint(3.5 * u, 20.5 * u);
    body.CloseSubpath();
    gc.StrokePath(body);

    // The metal shutter along the top, with its window.
    pen(1.6);
    gc.DrawRectangle(7.5 * u, 3 * u, 8 * u, 5.5 * u);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawRectangle(12 * u, 4.3 * u, 2 * u, 3 * u);

    // The label along the bottom, with a line written on it.
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    pen(1.6);
    gc.DrawRectangle(6.5 * u, 12.5 * u, 11.5 * u, 8 * u);
    pen(1.2);
    gc.StrokeLine(8.5 * u, 15.5 * u, 16 * u, 15.5 * u);
}

wxBitmapBundle floppyIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawFloppyIcon(gc, size, ink); }, sizeDip);
}

void drawPictureIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid, like the floppy beside it.
    const double u = size / 24.0;
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, 1.8 * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));

    // The frame.
    gc.DrawRoundedRectangle(3 * u, 4.5 * u, 18 * u, 15 * u, 1.5 * u);

    // The sun, up on the right.
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawEllipse(14.5 * u, 7.5 * u, 3.6 * u, 3.6 * u);

    // Two hills, the far one behind the near.
    wxGraphicsPath hills = gc.CreatePath();
    hills.MoveToPoint(4 * u, 18.5 * u);
    hills.AddLineToPoint(9.5 * u, 11 * u);
    hills.AddLineToPoint(13.5 * u, 16 * u);
    hills.AddLineToPoint(15.5 * u, 13.5 * u);
    hills.AddLineToPoint(20 * u, 18.5 * u);
    hills.CloseSubpath();
    gc.FillPath(hills);
}

wxBitmapBundle pictureIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawPictureIcon(gc, size, ink); }, sizeDip);
}

void drawMoonIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // A disc with a smaller one, up and to the right, cut out of it.
    const double u = size / 24.0;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawEllipse(3.5 * u, 3.5 * u, 17 * u, 17 * u);
    gc.SetCompositionMode(wxCOMPOSITION_CLEAR);
    gc.DrawEllipse(9 * u, 0.5 * u, 15 * u, 15 * u);
    gc.SetCompositionMode(wxCOMPOSITION_OVER);
}

void drawSunIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // A disc and eight rays.
    const double u = size / 24.0;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawEllipse(7 * u, 7 * u, 10 * u, 10 * u);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, 2 * u).Cap(wxCAP_ROUND)));
    for (int i = 0; i < 8; i++)
    {
        const double a = i * 3.14159265358979323846 / 4;
        gc.StrokeLine(12 * u + 7.5 * u * std::cos(a), 12 * u + 7.5 * u * std::sin(a),
                      12 * u + 10.5 * u * std::cos(a), 12 * u + 10.5 * u * std::sin(a));
    }
}

wxBitmapBundle themeIcon(bool dark, const wxColour& ink, int sizeDip)
{
    return drawnIcon(
        [dark, ink](wxGraphicsContext& gc, double size) {
            if (dark)
                drawSunIcon(gc, size, ink);
            else
                drawMoonIcon(gc, size, ink);
        },
        sizeDip);
}

void drawCheckIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid: a short stroke down to the left, a long one
    // up to the right.
    const double u = size / 24.0;
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, 3.0 * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    wxGraphicsPath check = gc.CreatePath();
    check.MoveToPoint(4.5 * u, 12.5 * u);
    check.AddLineToPoint(9.5 * u, 17.5 * u);
    check.AddLineToPoint(19.5 * u, 6.5 * u);
    gc.StrokePath(check);
}

wxBitmapBundle checkIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawCheckIcon(gc, size, ink); }, sizeDip);
}

void drawBellIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid: a bell's dome flaring to a lip, the knob on
    // top and the clapper under it.
    const double u = size / 24.0;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(gc.CreateBrush(wxBrush(ink)));
    wxGraphicsPath bell = gc.CreatePath();
    bell.MoveToPoint(12.0 * u, 3.5 * u);
    bell.AddCurveToPoint(7.0 * u, 3.5 * u, 6.0 * u, 8.0 * u, 6.0 * u, 12.0 * u);
    bell.AddCurveToPoint(6.0 * u, 15.0 * u, 4.5 * u, 16.5 * u, 3.5 * u, 17.5 * u);
    bell.AddLineToPoint(20.5 * u, 17.5 * u);
    bell.AddCurveToPoint(19.5 * u, 16.5 * u, 18.0 * u, 15.0 * u, 18.0 * u, 12.0 * u);
    bell.AddCurveToPoint(18.0 * u, 8.0 * u, 17.0 * u, 3.5 * u, 12.0 * u, 3.5 * u);
    bell.CloseSubpath();
    gc.FillPath(bell);
    gc.DrawEllipse(10.5 * u, 1.5 * u, 3.0 * u, 3.0 * u);
    gc.DrawEllipse(9.5 * u, 18.0 * u, 5.0 * u, 4.0 * u);
}

wxBitmapBundle bellIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawBellIcon(gc, size, ink); }, sizeDip);
}

void drawEarIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid: the rim of a right ear, from the top of the
    // helix round to the lobe, and the fold inside it.
    const double u = size / 24.0;
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, 2.2 * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    wxGraphicsPath rim = gc.CreatePath();
    rim.MoveToPoint(6.5 * u, 10.0 * u);
    rim.AddCurveToPoint(6.5 * u, 4.5 * u, 10.0 * u, 2.5 * u, 12.5 * u, 2.5 * u);
    rim.AddCurveToPoint(16.5 * u, 2.5 * u, 18.5 * u, 5.5 * u, 18.5 * u, 9.0 * u);
    rim.AddCurveToPoint(18.5 * u, 12.5 * u, 15.0 * u, 13.5 * u, 14.0 * u, 16.0 * u);
    rim.AddCurveToPoint(13.0 * u, 18.5 * u, 13.0 * u, 21.5 * u, 10.0 * u, 21.5 * u);
    rim.AddCurveToPoint(8.5 * u, 21.5 * u, 7.5 * u, 20.5 * u, 7.0 * u, 19.5 * u);
    gc.StrokePath(rim);
    wxGraphicsPath fold = gc.CreatePath();
    fold.MoveToPoint(10.0 * u, 10.0 * u);
    fold.AddCurveToPoint(10.0 * u, 7.0 * u, 11.5 * u, 6.0 * u, 12.8 * u, 6.0 * u);
    fold.AddCurveToPoint(14.5 * u, 6.0 * u, 15.2 * u, 7.5 * u, 15.2 * u, 9.0 * u);
    fold.AddCurveToPoint(15.2 * u, 10.5 * u, 13.5 * u, 11.0 * u, 12.5 * u, 12.5 * u);
    gc.StrokePath(fold);
}

wxBitmapBundle earIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawEarIcon(gc, size, ink); }, sizeDip);
}

void drawMicrophoneIcon(wxGraphicsContext& gc, double size, const wxColour& ink)
{
    // Drawn on a 24-unit grid: a capsule microphone on a yoke and stand.
    const double u = size / 24.0;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(ink));
    gc.DrawRoundedRectangle(8.0 * u, 2.0 * u, 8.0 * u, 13.0 * u, 4.0 * u);
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(ink, 2.0 * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    wxGraphicsPath yoke = gc.CreatePath();
    yoke.MoveToPoint(5.0 * u, 10.5 * u);
    yoke.AddCurveToPoint(5.0 * u, 15.5 * u, 8.5 * u, 18.5 * u, 12.0 * u, 18.5 * u);
    yoke.AddCurveToPoint(15.5 * u, 18.5 * u, 19.0 * u, 15.5 * u, 19.0 * u, 10.5 * u);
    gc.StrokePath(yoke);
    gc.StrokeLine(12.0 * u, 18.5 * u, 12.0 * u, 22.0 * u);
    gc.StrokeLine(8.0 * u, 22.0 * u, 16.0 * u, 22.0 * u);
}

wxBitmapBundle microphoneIcon(const wxColour& ink, int sizeDip)
{
    return drawnIcon([ink](wxGraphicsContext& gc, double size) { drawMicrophoneIcon(gc, size, ink); }, sizeDip);
}

wxString bellHtml(const wxWindow* window, int sizeDip, const wxColour& ink)
{
    int px = window->FromDIP(sizeDip);
    wxString name = wxString::Format("ragtime-bell-%s-%d.png", ink.GetAsString(wxC2S_HTML_SYNTAX).Mid(1), px);
    static std::set<wxString> made;
    if (made.count(name) == 0)
    {
        // SealBadge may have added the handler already: wx keeps one of each.
        if (!wxFileSystem::HasHandlerForPath("memory:x")) wxFileSystem::AddHandler(new wxMemoryFSHandler);
        if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
        wxBitmap bitmap = bellIcon(ink, sizeDip).GetBitmap(wxSize(px, px));
        if (!bitmap.IsOk()) return wxString();
        wxMemoryFSHandler::AddFile(name, bitmap.ConvertToImage(), wxBITMAP_TYPE_PNG);
        made.insert(name);
    }
    wxString side = wxString::Format("%d", px);
    return "<img src=\"memory:" + name + "\" width=\"" + side + "\" height=\"" + side + "\">";
}
