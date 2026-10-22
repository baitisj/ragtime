#include "GlobusToggle.h"

#include <algorithm>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>

namespace
{

// The mock-up tokens (docs/MOCKUPS.md): the accent blue of a toggle that is
// on, the border of one that is off, and the focus ring.
const wxColour ACCENT(0x1f, 0x5f, 0xbf);
const wxColour BORDER(0xb9, 0xbe, 0xc7);
const wxColour FOCUS(0xf0, 0xa0, 0x20);

constexpr int PAD_X = 8;
constexpr int PAD_Y = 4;
constexpr int ICON = 14;
constexpr int GAP = 5;

} // namespace

GlobusToggle::GlobusToggle(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS)
{
    SetLabel("Globus");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetToolTip("Show or hide the globe");
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &GlobusToggle::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &GlobusToggle::OnLeftDown, this);
    Bind(wxEVT_KEY_DOWN, &GlobusToggle::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
        Refresh();
        event.Skip();
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
        Refresh();
        event.Skip();
    });
    SetInitialSize();
}

void GlobusToggle::setOn(bool on)
{
    if (on == on_) return;
    on_ = on;
    Refresh();
}

wxSize GlobusToggle::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<GlobusToggle*>(this));
    dc.SetFont(GetFont());
    wxSize text = dc.GetTextExtent(GetLabel());
    return wxSize(PAD_X + ICON + GAP + text.x + PAD_X, PAD_Y + std::max(ICON, text.y) + PAD_Y);
}

void GlobusToggle::flip()
{
    on_ = !on_;
    Refresh();
    wxCommandEvent event(wxEVT_TOGGLEBUTTON, GetId());
    event.SetEventObject(this);
    event.SetInt(on_ ? 1 : 0);
    ProcessWindowEvent(event);
}

void GlobusToggle::OnLeftDown(wxMouseEvent&)
{
    SetFocus();
    flip();
}

void GlobusToggle::OnKeyDown(wxKeyEvent& event)
{
    int key = event.GetKeyCode();
    if (key == WXK_SPACE || key == WXK_RETURN || key == WXK_NUMPAD_ENTER)
    {
        flip();
        return;
    }
    if (key == WXK_TAB)
    {
        Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
        return;
    }
    event.Skip();
}

void GlobusToggle::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize size = GetClientSize();
    const wxColour ink = on_ ? *wxWHITE : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);

    // The button: filled blue when on, outlined when off.
    gc->SetPen(wxPen(on_ ? ACCENT : BORDER, 1));
    gc->SetBrush(on_ ? wxBrush(ACCENT) : *wxTRANSPARENT_BRUSH);
    gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, 4.0);
    if (HasFocus())
    {
        gc->SetPen(wxPen(FOCUS, 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(1.0, 1.0, size.x - 2.0, size.y - 2.0, 4.0);
    }

    // A small globe: its rim, the equator, and a meridian seen at a slant.
    const double r = ICON / 2.0 - 0.75;
    const double cx = PAD_X + ICON / 2.0;
    const double cy = size.y / 2.0;
    gc->SetPen(wxPen(ink, 1));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawEllipse(cx - r, cy - r, 2.0 * r, 2.0 * r);
    gc->StrokeLine(cx - r, cy, cx + r, cy);
    gc->DrawEllipse(cx - r * 0.45, cy - r, r * 0.9, 2.0 * r);
    gc->StrokeLine(cx - r * 0.85, cy - r * 0.5, cx + r * 0.85, cy - r * 0.5);
    gc->StrokeLine(cx - r * 0.85, cy + r * 0.5, cx + r * 0.85, cy + r * 0.5);

    gc->SetFont(GetFont(), ink);
    double tw = 0.0, th = 0.0;
    gc->GetTextExtent(GetLabel(), &tw, &th);
    gc->DrawText(GetLabel(), PAD_X + ICON + GAP, (size.y - th) / 2.0);
}
