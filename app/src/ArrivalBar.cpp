#include "ArrivalBar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include <wx/cursor.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/utils.h>

#include "Html.h"
#include "ragtime/picture/Picture.h"

namespace
{

constexpr int BAR_DIP = 12;     // the bar's height
constexpr int TICK_DIP = 4;     // the axes' ticks, over it and under it
constexpr int SIDE_DIP = 14;    // room each side for a label's half
constexpr int TURTLE_DIP = 66;  // the turtle's room, right of a burst bar
constexpr int BURST_DIP = 26;   // the burst's room between the bar and the turtle
constexpr int WIGGLE_MS = 350;  // each frame of its legs

// The turtle's colours: drawn, not themed, as a cartoon is.
const wxColour SHELL(0x4c, 0x9a, 0x3c), SHELL_LINE(0x2d, 0x5e, 0x22), BELLY(0xe6, 0xcf, 0x86),
    BELLY_LINE(0x9c, 0x85, 0x40), SKIN(0x8c, 0xc0, 0x63), TEAR(0x4a, 0xa3, 0xff);

} // namespace

ArrivalBar::ArrivalBar(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , wiggle_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(GetFont().Smaller());
    Bind(wxEVT_PAINT, &ArrivalBar::OnPaint, this);
    for (auto type : {wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_MOTION})
        Bind(type, &ArrivalBar::OnMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        if (!GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))) hover(false);
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        frame_ = 1 - frame_;
        Refresh();
    }, wiggle_.GetId());
}

void ArrivalBar::hover(bool over)
{
    if (!over) light(false);
    if (over == over_) return;
    over_ = over;
    SetCursor(over && whole_ > 0 ? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
    Refresh();
    if (onHover) onHover(over);
}

void ArrivalBar::light(bool lit)
{
    if (lit == lit_) return;
    lit_ = lit;
    Refresh();
    if (onLimits) onLimits(lit);
}

void ArrivalBar::seekTo(int x)
{
    if (whole_ == 0 || !onSeek) return;
    const double left = FromDIP(SIDE_DIP), width = std::max(1.0, GetClientSize().x - 2.0 * FromDIP(SIDE_DIP) - 1.0);
    double at = std::max(0.0, std::min(1.0, (x - left) / width));
    onSeek((uint64_t)std::llround(at * (double)whole_));
}

void ArrivalBar::OnMouse(wxMouseEvent& event)
{
    const wxEventType type = event.GetEventType();
    if (type == wxEVT_ENTER_WINDOW)
        hover(true);
    else if (type == wxEVT_LEAVE_WINDOW)
    {
        // Dragging, the bar keeps the mouse till the button comes up.
        if (!HasCapture()) hover(false);
    }
    else if (type == wxEVT_LEFT_DOWN)
    {
        hover(true);
        light(false);
        if (!HasCapture() && whole_ > 0) CaptureMouse();
        seekTo(event.GetX());
    }
    else if (type == wxEVT_MOTION)
    {
        hover(true);
        if (HasCapture() && event.LeftIsDown())
            seekTo(event.GetX());
        else
            light(timeFigure_.Contains(event.GetPosition()) || sizeFigure_.Contains(event.GetPosition()));
    }
    else if (type == wxEVT_LEFT_UP)
    {
        if (HasCapture()) ReleaseMouse();
        if (!GetClientRect().Contains(event.GetPosition())) hover(false);
    }
    event.Skip();
}

void ArrivalBar::setWhole(uint64_t whole, const std::vector<uint64_t>& burstEnds, double seconds)
{
    whole_ = whole;
    ends_ = burstEnds;
    seconds_ = seconds;
    slow_ = false;
    wiggle_.Stop();
    done_ = std::min(done_, whole_);
    Refresh();
}

void ArrivalBar::setDone(uint64_t done)
{
    done = std::min(done, whole_);
    if (done == done_) return;
    done_ = done;
    Refresh();
}

void ArrivalBar::setTooSlow(double seconds)
{
    whole_ = 0;
    done_ = 0;
    ends_.clear();
    seconds_ = seconds;
    slow_ = true;
    if (!wiggle_.IsRunning()) wiggle_.Start(WIGGLE_MS);
    Refresh();
}

wxSize ArrivalBar::DoGetBestClientSize() const
{
    return wxSize(FromDIP(120), FromDIP(BAR_DIP + 2 * TICK_DIP + 4) + 2 * GetCharHeight());
}

void ArrivalBar::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    timeFigure_ = sizeFigure_ = wxRect();
    if (whole_ == 0 && !slow_) return;
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize size = GetClientSize();
    const double left = FromDIP(SIDE_DIP);
    // Too slow, the bar leaves room on its right for the turtle.
    const double right = slow_ ? size.x - FromDIP(TURTLE_DIP) - FromDIP(BURST_DIP) : size.x - FromDIP(SIDE_DIP) - 1.0;
    const double width = std::max(1.0, right - left);
    const double tick = FromDIP(TICK_DIP);
    const double top = GetCharHeight() + tick + FromDIP(2);    // the bar's top, under the time axis
    const double bar = top + FromDIP(BAR_DIP);                  // and its foot
    auto xOf = [&](uint64_t bytes) { return left + width * (double)bytes / (double)std::max<uint64_t>(1, whole_); };
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);

    // The track, and what has come in it.
    gc->SetPen(wxPen(edge, 1));
    gc->SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    gc->DrawRectangle(left, top + 0.5, width, bar - top - 1.0);
    if (slow_)
    {
        // Too slow: red all along, bursting out of the right end, with
        // shards flying (Jeff, 2026-10-29).
        const double mid = (top + bar) / 2.0, d = FromDIP(1);
        wxGraphicsPath burst = gc->CreatePath();
        burst.MoveToPoint(left + 1.0, top + 1.5);
        burst.AddLineToPoint(right + 3 * d, top + 1.5);
        burst.AddLineToPoint(right + 9 * d, top - 4 * d);
        burst.AddLineToPoint(right + 7 * d, mid - 2 * d);
        burst.AddLineToPoint(right + 14 * d, mid);
        burst.AddLineToPoint(right + 7 * d, mid + 2 * d);
        burst.AddLineToPoint(right + 9 * d, bar + 4 * d);
        burst.AddLineToPoint(right + 3 * d, bar - 1.0);
        burst.AddLineToPoint(left + 1.0, bar - 1.0);
        burst.CloseSubpath();
        // Shards, as offsets from the burst's middle.
        for (const std::array<double, 6>& shard : {std::array<double, 6>{16, -11, 21, -13, 19, -8},
                                                   std::array<double, 6>{18, 10, 23, 12, 19, 14},
                                                   std::array<double, 6>{22, -2, 26, 0, 22, 2}})
        {
            burst.MoveToPoint(right + shard[0] * d, mid + shard[1] * d);
            burst.AddLineToPoint(right + shard[2] * d, mid + shard[3] * d);
            burst.AddLineToPoint(right + shard[4] * d, mid + shard[5] * d);
            burst.CloseSubpath();
        }
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(badText()));
        gc->FillPath(burst);
        drawTurtle(gc.get(), size.x - FromDIP(TURTLE_DIP), 0.0, FromDIP(TURTLE_DIP), size.y);
    }
    else if (done_ > 0)
    {
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(ACCENT_BLUE));
        gc->DrawRectangle(left + 1.0, top + 1.5, std::max(0.0, xOf(done_) - left - 1.0), bar - top - 2.5);
    }
    // Each burst's end, a thin line across the bar: what the far end gets
    // comes a burst at a time.
    gc->SetPen(wxPen(edge, 1));
    for (uint64_t end : ends_)
    {
        if (end == 0 || end >= whole_) continue;
        double x = std::round(xOf(end)) + 0.5;
        gc->StrokeLine(x, top + 1.0, x, bar - 1.0);
    }

    // Over the bar, a knob at how much has come, to drag.
    if (over_ && whole_ > 0)
    {
        const double knob = FromDIP(3);
        double x = xOf(done_);
        gc->SetPen(wxPen(ink, 1));
        gc->SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
        gc->DrawRoundedRectangle(x - knob, top - FromDIP(2), 2.0 * knob, bar - top + FromDIP(4), knob);
    }

    // The axes: ticks a round step apart, each labelled, as many as fit.
    // The largest figure on each is lit while the pointer is over either,
    // as the settings that make them are (Jeff, 2026-10-29).
    gc->SetPen(wxPen(ink, 1));
    wxFont bold = GetFont().Bold();
    auto label = [&](const std::string& text, double x, double y, bool largest) -> wxRect {
        wxString words = wxString::FromUTF8(text);
        gc->SetFont(largest && lit_ ? bold : GetFont(), largest && lit_ ? accentText() : ink);
        double w = 0, h = 0;
        gc->GetTextExtent(words, &w, &h);
        double at = std::max(0.0, std::min(size.x - w, x - w / 2.0));
        gc->DrawText(words, at, y);
        return wxRect((int)at, (int)y, (int)std::ceil(w), (int)std::ceil(h)).Inflate(FromDIP(3));
    };
    // Over the bar, the time: the bytes come at an even pace across the
    // whole, the gaps between bursts shared out, so seconds are even
    // along it too. Too slow, it runs to the airtime allowed.
    if (seconds_ > 0)
    {
        double timeWide = 0, timeHigh = 0;
        gc->SetFont(GetFont(), ink);
        gc->GetTextExtent(wxString::FromUTF8(ragtime::picture::timeLabel(seconds_, seconds_)), &timeWide, &timeHigh);
        int most = std::max(1, (int)(width / std::max(1.0, timeWide * 2.0)));
        std::vector<double> ticks = ragtime::picture::timeTicks(seconds_, most);
        for (size_t i = 0; i < ticks.size(); i++)
        {
            double x = std::round(left + width * ticks[i] / seconds_) + 0.5;
            gc->StrokeLine(x, top - tick, x, top);
            bool largest = i + 1 == ticks.size() && i > 0;
            wxRect where = label(ragtime::picture::timeLabel(ticks[i], seconds_), x, 0.0, largest);
            if (largest) timeFigure_ = where;
        }
    }
    // Under it, the bytes.
    if (whole_ == 0) return;
    double labelWide = 0, high = 0;
    gc->SetFont(GetFont(), ink);
    gc->GetTextExtent(wxString::FromUTF8(ragtime::picture::scaleLabel(whole_, whole_)), &labelWide, &high);
    int most = std::max(1, (int)(width / std::max(1.0, labelWide * 2.0)));
    std::vector<uint64_t> ticks = ragtime::picture::scaleTicks(whole_, most);
    for (size_t i = 0; i < ticks.size(); i++)
    {
        double x = std::round(xOf(ticks[i])) + 0.5;
        gc->StrokeLine(x, bar + 1.0, x, bar + tick);
        bool largest = i + 1 == ticks.size() && i > 0;
        wxRect where = label(ragtime::picture::scaleLabel(ticks[i], whole_), x, bar + tick + 1.0, largest);
        if (largest) sizeFigure_ = where;
    }
}

void ArrivalBar::drawTurtle(wxGraphicsContext* gc, double x, double y, double w, double h) const
{
    // Drawn on a 64 by 48 grid, scaled to fit and centred.
    const double s = std::min(w / 64.0, h / 48.0);
    gc->PushState();
    gc->Translate(x + (w - 64.0 * s) / 2.0, y + (h - 48.0 * s) / 2.0);
    gc->Scale(s, s);
    const double flip = frame_ == 0 ? 1.0 : -1.0;

    // The legs, kicking at the sky: outlined, then filled, all leaning
    // one way and then the other, each kicking higher than the next.
    const double legs[] = {17, 26, 38, 47};
    auto tip = [&](int i) { return wxPoint2DDouble(legs[i] + 3.5 * flip, (i + frame_) % 2 == 0 ? 6.0 : 9.0); };
    for (int pass = 0; pass < 2; pass++)
    {
        gc->SetPen(gc->CreatePen(
            wxGraphicsPenInfo(pass == 0 ? SHELL_LINE : SKIN, pass == 0 ? 6.0 : 4.2).Cap(wxCAP_ROUND)));
        for (int i = 0; i < 4; i++) gc->StrokeLine(legs[i], 21.0, tip(i).m_x, tip(i).m_y);
    }
    // Little marks by the feet, for the wiggle.
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(0x9a, 0x9a, 0x9a), 1.0).Cap(wxCAP_ROUND)));
    for (int i = 0; i < 4; i++)
    {
        double side = -4.5 * flip;
        gc->StrokeLine(tip(i).m_x + side, tip(i).m_y - 2.0, tip(i).m_x + side * 1.5, tip(i).m_y - 4.0);
        gc->StrokeLine(tip(i).m_x + side, tip(i).m_y + 1.5, tip(i).m_x + side * 1.6, tip(i).m_y + 1.5);
    }

    // The tail, stuck out.
    wxGraphicsPath tail = gc->CreatePath();
    tail.MoveToPoint(53.0, 21.0);
    tail.AddLineToPoint(60.0, 18.0 + flip);
    tail.AddLineToPoint(53.0, 25.0);
    tail.CloseSubpath();
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(SHELL_LINE, 1.2)));
    gc->SetBrush(wxBrush(SKIN));
    gc->DrawPath(tail);

    // The shell underneath, its dome to the ground, and its plates.
    wxGraphicsPath shell = gc->CreatePath();
    shell.MoveToPoint(10.0, 24.0);
    shell.AddCurveToPoint(10.0, 47.0, 54.0, 47.0, 54.0, 24.0);
    shell.CloseSubpath();
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(SHELL_LINE, 1.5)));
    gc->SetBrush(wxBrush(SHELL));
    gc->DrawPath(shell);
    wxGraphicsPath plates = gc->CreatePath();
    plates.MoveToPoint(13.0, 32.0);
    plates.AddCurveToPoint(24.0, 36.0, 40.0, 36.0, 51.0, 32.0);
    plates.MoveToPoint(22.0, 25.0);
    plates.AddLineToPoint(24.0, 35.0);
    plates.MoveToPoint(32.0, 25.0);
    plates.AddLineToPoint(32.0, 36.0);
    plates.MoveToPoint(42.0, 25.0);
    plates.AddLineToPoint(40.0, 35.0);
    plates.MoveToPoint(28.0, 36.0);
    plates.AddLineToPoint(28.0, 41.5);
    plates.MoveToPoint(36.0, 36.0);
    plates.AddLineToPoint(36.0, 41.5);
    gc->StrokePath(plates);

    // The belly, up.
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(BELLY_LINE, 1.2)));
    gc->SetBrush(wxBrush(BELLY));
    gc->DrawRoundedRectangle(11.0, 19.5, 42.0, 6.0, 3.0);

    // The head, thrown back, eyes shut tight, mouth turned down.
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(SHELL_LINE, 1.2)));
    gc->SetBrush(wxBrush(SKIN));
    gc->DrawEllipse(-1.0, 15.5 + 0.5 * flip, 15.0, 14.0);
    wxGraphicsPath face = gc->CreatePath();
    face.MoveToPoint(2.0, 20.5);
    face.AddCurveToPoint(4.0, 18.5, 6.5, 18.5, 8.5, 20.5);
    face.MoveToPoint(3.0, 26.5);
    face.AddCurveToPoint(5.0, 24.0, 8.0, 24.0, 10.0, 26.5);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(SHELL_LINE, 1.1).Cap(wxCAP_ROUND)));
    gc->StrokePath(face);
    // Tears, falling: one just welling and one on its way in turn.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(TEAR));
    auto tear = [&](double cx, double cy, double r) {
        wxGraphicsPath drop = gc->CreatePath();
        drop.MoveToPoint(cx, cy - 2.2 * r);
        drop.AddCurveToPoint(cx + 1.3 * r, cy - 0.6 * r, cx + r, cy + r, cx, cy + r);
        drop.AddCurveToPoint(cx - r, cy + r, cx - 1.3 * r, cy - 0.6 * r, cx, cy - 2.2 * r);
        drop.CloseSubpath();
        gc->FillPath(drop);
    };
    if (frame_ == 0)
    {
        tear(2.0, 23.5, 1.5);
        tear(0.5, 33.0, 1.9);
    }
    else
    {
        tear(2.5, 23.0, 1.2);
        tear(0.0, 28.5, 1.9);
        tear(9.5, 23.0, 1.2);
    }
    gc->PopState();
}
