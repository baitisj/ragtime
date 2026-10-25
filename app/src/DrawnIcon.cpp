#include "DrawnIcon.h"

#include <cmath>
#include <memory>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/image.h>

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
