#include "EarControls.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

namespace
{

constexpr int TICK_MS = 40;
constexpr long STORED_MS = 1200;
constexpr double PI = 3.14159265358979323846;

// The wedge of a kitchen timer: from twelve o'clock, clockwise, the share left.
void drawClock(wxGraphicsContext* gc, double x, double y, double r, double left, const wxColour& colour,
               const wxColour& face)
{
    // Cairo shifts a 1 px stroke half a pixel down and right but not a fill,
    // which left the face off centre in its ring: no shift for either.
    const bool offset = gc->OffsetEnabled();
    gc->DisableOffset();
    gc->SetPen(wxPen(colour, 1));
    gc->SetBrush(wxBrush(face));
    gc->DrawEllipse(x - r, y - r, 2 * r, 2 * r);
    left = std::max(0.0, std::min(1.0, left));
    if (left > 0) {
        wxGraphicsPath wedge = gc->CreatePath();
        wedge.MoveToPoint(x, y);
        wedge.AddArc(x, y, r - 1.5, -PI / 2, -PI / 2 + left * 2 * PI, true);
        wedge.CloseSubpath();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(colour));
        gc->FillPath(wedge);
    }
    if (offset) gc->EnableOffset();
}

} // namespace

ClockDial::ClockDial(wxWindow* parent, int sizeDip)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , size_(sizeDip)
    , colour_(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
}

void ClockDial::setLeft(double left)
{
    if (std::fabs(left - left_) < 0.002) return;
    left_ = left;
    Refresh();
}

void ClockDial::setColour(const wxColour& colour)
{
    colour_ = colour;
    Refresh();
}

wxSize ClockDial::DoGetBestClientSize() const
{
    return FromDIP(wxSize(size_, size_));
}

void ClockDial::paint()
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    const wxSize s = GetClientSize();
    const double r = std::min(s.x, s.y) / 2.0 - 1;
    drawClock(gc.get(), s.x / 2.0, s.y / 2.0, r, left_, colour_, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
}

PresetButton::PresetButton(wxWindow* parent, const wxString& name)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , name_(name)
    , detail_(wxString::FromUTF8("\xe2\x80\x94"))
    , timer_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (!IsEnabled()) return;
        heldMs_ = 0;
        storedMs_ = -1;
        CaptureMouse();
        timer_.Start(TICK_MS);
        Refresh();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        if (HasCapture()) ReleaseMouse();
        const bool tapped = heldMs_ >= 0 && wxRect(GetClientSize()).Contains(e.GetPosition());
        heldMs_ = -1;
        if (storedMs_ < 0) timer_.Stop();
        Refresh();
        if (tapped && tap_) tap_();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        heldMs_ = -1;
        Refresh();
    });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
        hover_ = true;
        Refresh();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        hover_ = false;
        Refresh();
    });
}

void PresetButton::setDetail(const wxString& detail)
{
    if (detail == detail_) return;
    detail_ = detail;
    InvalidateBestSize();
    Refresh();
}

void PresetButton::tick()
{
    if (heldMs_ >= 0)
    {
        heldMs_ += TICK_MS;
        if (heldMs_ >= HOLD_MS)
        {
            // Held long enough: stored, and the release is no tap.
            heldMs_ = -1;
            if (HasCapture()) ReleaseMouse();
            storedMs_ = 0;
            if (store_) store_();
        }
    }
    else if (storedMs_ >= 0)
    {
        storedMs_ += TICK_MS;
        if (storedMs_ >= STORED_MS)
        {
            storedMs_ = -1;
            timer_.Stop();
        }
    }
    else
    {
        timer_.Stop();
    }
    Refresh();
}

wxSize PresetButton::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<PresetButton*>(this));
    dc.SetFont(GetFont());
    const wxSize a = dc.GetTextExtent(name_);
    dc.SetFont(GetFont().Smaller());
    const wxString widest = wxString::FromUTF8("0000\xe2\x80\x93" "0000");
    const wxSize b = dc.GetTextExtent(detail_.length() < widest.length() ? widest : detail_);
    return wxSize(std::max(a.x, b.x) + FromDIP(30), a.y + b.y + FromDIP(8));
}

void PresetButton::paint()
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    const wxSize s = GetClientSize();
    const bool on = IsEnabled();
    const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    const wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW);
    const wxColour text = on ? wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)
                             : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    const wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    gc->SetPen(wxPen(heldMs_ >= 0 ? accent : edge, 1));
    gc->SetBrush(wxBrush(on && (hover_ || heldMs_ >= 0) ? face.ChangeLightness(face.GetLuminance() < 0.5 ? 115 : 95)
                                                        : face));
    gc->DrawRoundedRectangle(0.5, 0.5, s.x - 1, s.y - 1, FromDIP(4));

    const wxString detail = storedMs_ >= 0 ? wxString("Stored") : detail_;
    double w, h, w2, h2;
    gc->SetFont(GetFont(), text);
    gc->GetTextExtent(name_, &w, &h);
    gc->SetFont(GetFont().Smaller(), text);
    gc->GetTextExtent(detail, &w2, &h2);
    const double top = (s.y - h - h2) / 2;
    gc->SetFont(GetFont().Bold(), text);
    gc->DrawText(name_, (s.x - w) / 2, top);
    gc->SetFont(GetFont().Smaller(), storedMs_ >= 0 ? accent : text);
    gc->DrawText(detail, (s.x - w2) / 2, top + h);

    // Held: the quick clock in the corner.
    if (heldMs_ >= 0)
    {
        const double r = FromDIP(6);
        drawClock(gc.get(), s.x - r - FromDIP(4), r + FromDIP(4), r, 1.0 - (double)heldMs_ / HOLD_MS, accent,
                  wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    }
}
