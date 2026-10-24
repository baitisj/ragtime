#include "GearshiftIcon.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/image.h>

namespace
{

constexpr double PI = 3.14159265358979323846;

// Where something drawn landed: the box of pixels with any ink.
struct InkBox
{
    int left = 0, top = 0, right = -1, bottom = -1;
    int width() const { return right - left + 1; }
    int height() const { return bottom - top + 1; }
};

// Draws into a clear w by h image.
wxImage drawImage(int w, int h, const std::function<void(wxGraphicsContext&)>& draw)
{
    wxImage image(w, h);
    image.InitAlpha();
    std::fill(image.GetAlpha(), image.GetAlpha() + w * h, 0);
    {
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
        if (gc)
        {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            draw(*gc);
        }
    }
    return image;
}

// The box of pixels at least a quarter inked, so the faintest edge of the
// antialiasing doesn't open the gap between dial and name.
InkBox inkBox(const wxImage& image)
{
    InkBox box;
    box.left = image.GetWidth();
    box.top = image.GetHeight();
    const unsigned char* alpha = image.GetAlpha();
    for (int y = 0; y < image.GetHeight(); y++)
        for (int x = 0; x < image.GetWidth(); x++)
            if (alpha[y * image.GetWidth() + x] >= 64)
            {
                box.left = std::min(box.left, x);
                box.right = std::max(box.right, x);
                box.top = std::min(box.top, y);
                box.bottom = std::max(box.bottom, y);
            }
    if (box.right < 0) box = InkBox{};
    return box;
}

// Copies the inked box of from into to with its top left at (x, y), and
// the faint pixel around it too where to has room.
void copyInk(const wxImage& from, const InkBox& box, wxImage& to, int x, int y)
{
    for (int row = -1; row <= box.height(); row++)
        for (int col = -1; col <= box.width(); col++)
        {
            const int tx = x + col, ty = y + row;
            const int fx = box.left + col, fy = box.top + row;
            if (tx < 0 || ty < 0 || tx >= to.GetWidth() || ty >= to.GetHeight()) continue;
            if (fx < 0 || fy < 0 || fx >= from.GetWidth() || fy >= from.GetHeight()) continue;
            if (from.GetAlpha(fx, fy) <= to.GetAlpha(tx, ty)) continue;
            to.SetRGB(tx, ty, from.GetRed(fx, fy), from.GetGreen(fx, fy), from.GetBlue(fx, fy));
            to.SetAlpha(tx, ty, from.GetAlpha(fx, fy));
        }
}

} // namespace

void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink, const wxColour& needleInk,
                       double needleAt)
{
    // Drawn on a 24-unit grid, in fine lines.
    const double u = size / 24.0;
    auto pen = [&](const wxColour& colour, double width) {
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, width * u).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    };
    gc.SetBrush(*wxTRANSPARENT_BRUSH);

    // A dial from lower left round to lower right, 240 degrees, with five
    // ticks, and the needle needleAt of the way round it.
    const double cx = 12 * u, cy = 13.5 * u, r = 10.5 * u;
    const double start = PI * 5 / 6, sweep = PI * 4 / 3;
    pen(ink, 1.2);
    wxGraphicsPath arc = gc.CreatePath();
    arc.AddArc(cx, cy, r, start, PI / 6, true);
    gc.StrokePath(arc);
    pen(ink, 1.0);
    for (int i = 0; i < 5; i++)
    {
        const double a = start + i * sweep / 4;
        gc.StrokeLine(cx + std::cos(a) * r * 0.64, cy + std::sin(a) * r * 0.64,
                      cx + std::cos(a) * r * 0.84, cy + std::sin(a) * r * 0.84);
    }
    pen(needleInk, 1.4);
    const double needle = start + std::clamp(needleAt, 0.0, 1.0) * sweep;
    gc.StrokeLine(cx, cy, cx + std::cos(needle) * r * 0.78, cy + std::sin(needle) * r * 0.78);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(needleInk));
    gc.DrawEllipse(cx - 1.8 * u, cy - 1.8 * u, 3.6 * u, 3.6 * u);
}

wxBitmapBundle gearButtonIcon(const wxColour& ink, const wxColour& needleInk, double needleAt, const wxFont& font,
                              const wxString& name, int dialDip)
{
    // The dial and the name are drawn apart, each measured by where its
    // ink falls, then put together with the name's tops NAME_GAP + 1 px
    // under the dial's foot (Jeff, 2026-10-23: "within 2 px"). A button's
    // own label sits too far under its bitmap for that, so the name is
    // part of the picture.
    constexpr int NAME_GAP = 1;
    auto drawName = [&](const wxFont& nameFont, int pad) {
        double textW = 0, textH = 0;
        drawImage(1, 1, [&](wxGraphicsContext& gc) {
            gc.SetFont(nameFont, ink);
            gc.GetTextExtent(name, &textW, &textH);
        });
        return drawImage(int(std::ceil(textW)) + 2 * pad, int(std::ceil(textH)) + 2 * pad,
                         [&](wxGraphicsContext& gc) {
                             gc.SetFont(nameFont, ink);
                             gc.DrawText(name, pad, pad);
                         });
    };

    wxVector<wxBitmap> bitmaps;
    int width1 = 0, height1 = 0, nameHeight1 = 0;
    for (int scale : {1, 2})
    {
        const int dialPx = dialDip * scale;
        const wxImage dial = drawImage(dialPx, dialPx, [&](wxGraphicsContext& gc) {
            drawGearshiftIcon(gc, dialPx, ink, needleInk, needleAt);
        });
        const InkBox dialBox = inkBox(dial);

        // Text doesn't scale exactly: at 2x, step the font down until the
        // name is no taller than twice the 1x one.
        float fontScale = float(scale);
        wxImage text = drawName(font.Scaled(fontScale), 2 * scale);
        InkBox textBox = inkBox(text);
        for (int i = 0; scale > 1 && i < 12 && textBox.height() > nameHeight1 * scale; i++)
        {
            fontScale *= 0.97f;
            text = drawName(font.Scaled(fontScale), 2 * scale);
            textBox = inkBox(text);
        }

        if (scale == 1)
        {
            width1 = std::max(dialBox.width(), textBox.width());
            height1 = dialBox.height() + NAME_GAP + textBox.height();
            nameHeight1 = textBox.height();
        }
        // The 2x picture is exactly twice the 1x one, so the two agree:
        // the dial at the top, the name at the foot.
        wxImage out = drawImage(width1 * scale, height1 * scale, [](wxGraphicsContext&) {});
        out.SetRGB(wxRect(0, 0, out.GetWidth(), out.GetHeight()), ink.Red(), ink.Green(), ink.Blue());
        copyInk(dial, dialBox, out, (out.GetWidth() - dialBox.width()) / 2, 0);
        copyInk(text, textBox, out, (out.GetWidth() - textBox.width()) / 2, out.GetHeight() - textBox.height());
        bitmaps.push_back(wxBitmap(out));
    }
    return wxBitmapBundle::FromBitmaps(bitmaps);
}
