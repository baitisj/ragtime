#include "DrawnIcon.h"

#include <memory>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/image.h>

wxBitmapBundle drawnIcon(const DrawIcon& draw, int sizeDip)
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
