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
constexpr int BURST_DIP = 28;   // the burst's room, right of a bar too slow
constexpr int WALK_MS = 40;     // the grey lines move a pixel this often
constexpr int STRIPE_DIP = 9;   // and are this far apart

} // namespace

ArrivalBar::ArrivalBar(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , walker_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(GetFont().Smaller());
    Bind(wxEVT_PAINT, &ArrivalBar::OnPaint, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        walk_++;
        if (encoded_ > 0.0) Refresh();
    });
    for (auto type : {wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_MOTION})
        Bind(type, &ArrivalBar::OnMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        if (!GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))) hover(false);
    });
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

void ArrivalBar::stopEncoding()
{
    encoding_ = false;
    encoded_ = 0.0;
    walker_.Stop();
}

void ArrivalBar::setEncoding(double done)
{
    whole_ = 0;
    done_ = 0;
    ends_.clear();
    seconds_ = 0.0;
    slow_ = false;
    encoded_ = std::max(0.0, std::min(1.0, done));
    if (!encoding_)
    {
        encoding_ = true;
        walker_.Start(WALK_MS);
    }
    Refresh();
}

void ArrivalBar::setWhole(uint64_t whole, const std::vector<uint64_t>& burstEnds, double seconds)
{
    stopEncoding();
    whole_ = whole;
    ends_ = burstEnds;
    seconds_ = seconds;
    slow_ = false;
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
    stopEncoding();
    whole_ = 0;
    done_ = 0;
    ends_.clear();
    seconds_ = seconds;
    slow_ = true;
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
    if (whole_ == 0 && !slow_ && !encoding_) return;
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize size = GetClientSize();
    const double left = FromDIP(SIDE_DIP);
    // Too slow, the bar leaves room on its right for the burst.
    const double right = slow_ ? size.x - FromDIP(BURST_DIP) : size.x - FromDIP(SIDE_DIP) - 1.0;
    const double width = std::max(1.0, right - left);
    const double tick = FromDIP(TICK_DIP);
    const double top = GetCharHeight() + tick + FromDIP(2);    // the bar's top, under the time axis
    const double bar = top + FromDIP(BAR_DIP);                  // and its foot
    auto xOf = [&](uint64_t bytes) { return left + width * (double)bytes / (double)std::max<uint64_t>(1, whole_); };
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
    if (encoding_)
    {
        paintEncoding(*gc, left, width, top, bar);
        return;
    }

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

void ArrivalBar::paintEncoding(wxGraphicsContext& gc, double left, double width, double top, double bar)
{
    const wxSize size = GetClientSize();
    const double tick = FromDIP(TICK_DIP);
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
    const double right = left + width;

    gc.SetPen(wxPen(edge, 1));
    gc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    gc.DrawRectangle(left, top + 0.5, width, bar - top - 1.0);
    // The work done fills from the right, as the axis under it counts up
    // to 100% at the left.
    const double inner = std::max(0.0, width - 1.0);
    const double from = right - inner * encoded_;
    if (encoded_ > 0.0 && right - from >= 0.5)
    {
        const double y0 = top + 1.5, h = bar - top - 2.5;
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(ACCENT_BLUE));
        gc.DrawRectangle(from, y0, right - from, h);
        // Grey lines leaning across it, walking toward the 100% end: the
        // computer is at work.
        gc.Clip(from, y0, right - from, h);
        const double gap = FromDIP(STRIPE_DIP), lean = h;
        const double offset = std::fmod((double)walk_, gap);
        gc.SetPen(wxPen(wxColour(0xc8, 0xc8, 0xc8, 0x90), std::max(1, FromDIP(3))));
        for (double x = right + lean - offset; x > from - gap; x -= gap)
            gc.StrokeLine(x, y0, x - lean, y0 + h);
        gc.ResetClip();
    }

    // Under it, the percent: 0% on the right, 100% on the left, as many
    // round steps as fit.
    gc.SetPen(wxPen(ink, 1));
    gc.SetFont(GetFont(), ink);
    double wide = 0, high = 0;
    gc.GetTextExtent("100%", &wide, &high);
    int step = 100;
    for (int s : {10, 20, 25, 50})
        if ((100 / s) * wide * 2.0 <= width)
        {
            step = s;
            break;
        }
    for (int p = 0; p <= 100; p += step)
    {
        const double x = std::round(right - inner * p / 100.0) + 0.5;
        gc.StrokeLine(x, bar + 1.0, x, bar + tick);
        wxString words = wxString::Format("%d%%", p);
        double w = 0, h = 0;
        gc.GetTextExtent(words, &w, &h);
        gc.DrawText(words, std::max(0.0, std::min(size.x - w, x - w / 2.0)), bar + tick + 1.0);
    }
}
