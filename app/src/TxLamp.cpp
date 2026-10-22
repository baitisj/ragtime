#include "TxLamp.h"

#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

namespace
{

const wxColour LIT(0xc8, 0x28, 0x1e);   // the Globus path's red

} // namespace

TxLamp::TxLamp(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetToolTip("Lit while data2g-host is transmitting");
    Bind(wxEVT_PAINT, &TxLamp::OnPaint, this);
}

void TxLamp::setOn(bool on)
{
    if (on_ == on) return;
    on_ = on;
    Refresh();
}

wxSize TxLamp::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<TxLamp*>(this));
    dc.SetFont(GetFont().Bold());
    wxSize text = dc.GetTextExtent("TX Off");
    return wxSize(text.x + FromDIP(20), text.y + FromDIP(8));
}

void TxLamp::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    wxSize size = GetClientSize();
    double inset = 1.0;
    double radius = (size.y - 2 * inset) / 2.0;
    wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    if (on_)
    {
        gc->SetBrush(wxBrush(LIT));
        gc->SetPen(wxPen(LIT.ChangeLightness(80), 1));
        text = *wxWHITE;
    }
    else
    {
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(edge, 1));
        text = edge;
    }
    gc->DrawRoundedRectangle(inset, inset, size.x - 2 * inset, size.y - 2 * inset, radius);

    wxFont font = GetFont().Bold();
    gc->SetFont(font, text);
    wxString label = on_ ? "TX" : "TX Off";
    double w = 0, h = 0;
    gc->GetTextExtent(label, &w, &h);
    gc->DrawText(label, (size.x - w) / 2.0, (size.y - h) / 2.0);
}
