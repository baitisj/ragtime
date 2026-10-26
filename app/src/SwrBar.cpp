#include "SwrBar.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

#include "ragtime/radio/TxMeters.h"

using ragtime::radio::SwrBand;

namespace
{

wxColour bandColour(SwrBand band)
{
    switch (band)
    {
    case SwrBand::Good: return wxColour(0x2e, 0x9d, 0x3a);
    case SwrBand::Fair: return wxColour(0xe0, 0xc2, 0x00);
    case SwrBand::Poor: return wxColour(0xf0, 0x8a, 0x00);
    case SwrBand::Bad: break;
    }
    return wxColour(0xd6, 0x28, 0x28);
}

// The marks: 1:1 is the left end.
const double MARKS[] = {1.0, 1.5, 2.0, 3.0};

} // namespace

SwrBar::SwrBar(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &SwrBar::OnPaint, this);
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (tripped_ && clear_) clear_();
    });
}

void SwrBar::setSwr(std::optional<double> swr)
{
    if (swr == swr_) return;
    swr_ = swr;
    Refresh();
}

void SwrBar::setTripped(bool tripped)
{
    if (tripped == tripped_) return;
    tripped_ = tripped;
    SetCursor(tripped ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    Refresh();
}

wxSize SwrBar::DoGetBestClientSize() const
{
    return FromDIP(wxSize(60, 9));
}

void SwrBar::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    wxSize size = GetClientSize();
    double w = size.x - 1.0, h = size.y - 1.0;
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    wxColour track = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour marks = wxSystemSettings::GetColour(IsEnabled() ? wxSYS_COLOUR_BTNTEXT : wxSYS_COLOUR_GRAYTEXT);

    gc->SetPen(wxPen(edge, 1));
    gc->SetBrush(wxBrush(track));
    gc->DrawRectangle(0.5, 0.5, w, h);

    if (swr_ && IsEnabled())
    {
        double fill = ragtime::radio::swrScale(*swr_) * (w - 1);
        if (fill > 0)
        {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(bandColour(ragtime::radio::swrBand(*swr_))));
            gc->DrawRectangle(1, 1, fill, h - 1);
        }
    }

    // Small marks up from the foot, at 1, 1.5, 2 and 3.
    gc->SetPen(wxPen(marks, 1));
    double tall = std::max(2.0, h * 0.45);
    for (double mark : MARKS)
    {
        double x = 1 + std::max(1.0, ragtime::radio::swrScale(mark) * (w - 1));
        gc->StrokeLine(x, h - tall, x, h);
    }

    if (tripped_)
    {
        gc->SetPen(wxPen(bandColour(SwrBand::Bad), 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRectangle(1, 1, w - 1, h - 1);
    }
}
