#include "ToggleSwitch.h"

#include <algorithm>
#include <memory>

#include <wx/brush.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>

#include "Html.h"

namespace
{

// The track, in DIPs.
constexpr int TRACK_WIDTH = 30;
constexpr int TRACK_HEIGHT = 16;
constexpr int GAP = 6;  // between the track and the label

} // namespace

ToggleSwitch::ToggleSwitch(wxWindow* parent, wxWindowID id, const wxString& label)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS)
{
    SetLabel(label);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ToggleSwitch::OnPaint, this);
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (IsEnabled()) flip();
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        SetFocus();
        e.Skip();
    });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        hover_ = true;
        Refresh();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        hover_ = false;
        Refresh();
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_SPACE && IsEnabled())
            flip();
        else
            e.Skip();
    });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        Refresh();
        e.Skip();
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        Refresh();
        e.Skip();
    });
}

void ToggleSwitch::SetValue(bool on)
{
    if (on_ == on) return;
    on_ = on;
    Refresh();
}

void ToggleSwitch::flip()
{
    on_ = !on_;
    Refresh();
    wxCommandEvent event(wxEVT_TOGGLEBUTTON, GetId());
    event.SetEventObject(this);
    event.SetInt(on_ ? 1 : 0);
    ProcessWindowEvent(event);
}

wxSize ToggleSwitch::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<ToggleSwitch*>(this));
    dc.SetFont(GetFont());
    const wxSize text = dc.GetTextExtent(GetLabel());
    return wxSize(FromDIP(TRACK_WIDTH + GAP + 4) + text.x,
                  std::max(text.y, FromDIP(TRACK_HEIGHT)) + FromDIP(8));
}

void ToggleSwitch::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
#ifndef __WXGTK__
    // GTK has drawn the bar's own background underneath already; its
    // GetBackgroundColour() is a shade off what the theme paints.
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
#endif
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize size = GetClientSize();
    const wxColour back = GetParent()->GetBackgroundColour();
    const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const double w = FromDIP(TRACK_WIDTH), h = FromDIP(TRACK_HEIGHT);
    const double x = FromDIP(2), y = (size.y - h) / 2;
    const bool enabled = IsEnabled();

    // The track: blue when on, a soft grey when off.
    wxColour track = on_ ? ACCENT_BLUE : blendColour(back, ink, hover_ ? 0.40 : 0.30);
    if (!enabled) track = blendColour(back, track, 0.45);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(track));
    gc->DrawRoundedRectangle(x, y, w, h, h / 2);

    // The knob, white, at the end it is switched to.
    const double inset = FromDIP(2);
    const double d = h - 2 * inset;
    const double kx = on_ ? x + w - inset - d : x + inset;
    gc->SetBrush(*wxWHITE_BRUSH);
    gc->DrawEllipse(kx, y + inset, d, d);

    if (HasFocus())
    {
        gc->SetPen(wxPen(blendColour(back, ACCENT_BLUE, 0.6), FromDIP(1)));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(x - 1.5, y - 1.5, w + 3, h + 3, h / 2 + 1.5);
    }

    gc->SetFont(GetFont(), enabled ? ink : blendColour(back, ink, 0.45));
    double tw = 0, th = 0;
    gc->GetTextExtent(GetLabel(), &tw, &th);
    gc->DrawText(GetLabel(), x + w + FromDIP(GAP), (size.y - th) / 2);
}
