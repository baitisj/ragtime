#include "ArrivalBar.h"

#include <algorithm>
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

} // namespace

ArrivalBar::ArrivalBar(wxWindow* parent, wxWindowID id)
    : wxControl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(GetFont().Smaller());
    Bind(wxEVT_PAINT, &ArrivalBar::OnPaint, this);
    for (auto type : {wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_MOTION})
        Bind(type, &ArrivalBar::OnMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        if (!GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))) hover(false);
    });
}

void ArrivalBar::hover(bool over)
{
    if (over == over_) return;
    over_ = over;
    SetCursor(over && whole_ > 0 ? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
    Refresh();
    if (onHover) onHover(over);
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
        if (!HasCapture()) CaptureMouse();
        seekTo(event.GetX());
    }
    else if (type == wxEVT_MOTION)
    {
        hover(true);
        if (HasCapture() && event.LeftIsDown()) seekTo(event.GetX());
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

wxSize ArrivalBar::DoGetBestClientSize() const
{
    return wxSize(FromDIP(120), FromDIP(BAR_DIP + 2 * TICK_DIP + 4) + 2 * GetCharHeight());
}

void ArrivalBar::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    if (whole_ == 0) return;
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;

    const wxSize size = GetClientSize();
    const double left = FromDIP(SIDE_DIP), right = size.x - FromDIP(SIDE_DIP) - 1.0;
    const double width = std::max(1.0, right - left);
    const double tick = FromDIP(TICK_DIP);
    const double top = GetCharHeight() + tick + FromDIP(2);    // the bar's top, under the time axis
    const double bar = top + FromDIP(BAR_DIP);                  // and its foot
    auto xOf = [&](uint64_t bytes) { return left + width * (double)bytes / (double)whole_; };
    wxColour edge = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);

    // The track, and what has come in it.
    gc->SetPen(wxPen(edge, 1));
    gc->SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    gc->DrawRectangle(left, top + 0.5, width, bar - top - 1.0);
    if (done_ > 0)
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
    if (over_)
    {
        const double knob = FromDIP(3);
        double x = xOf(done_);
        gc->SetPen(wxPen(ink, 1));
        gc->SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
        gc->DrawRoundedRectangle(x - knob, top - FromDIP(2), 2.0 * knob, bar - top + FromDIP(4), knob);
    }

    // The axes: ticks a round step apart, each labelled, as many as fit.
    gc->SetFont(GetFont(), ink);
    gc->SetPen(wxPen(ink, 1));
    auto label = [&](const std::string& text, double x, double y) {
        wxString words = wxString::FromUTF8(text);
        double w = 0, h = 0;
        gc->GetTextExtent(words, &w, &h);
        gc->DrawText(words, std::max(0.0, std::min(size.x - w, x - w / 2.0)), y);
    };
    // Over the bar, the time: the bytes come at an even pace across the
    // whole, the gaps between bursts shared out, so seconds are even
    // along it too.
    if (seconds_ > 0)
    {
        double timeWide = 0, timeHigh = 0;
        gc->GetTextExtent(wxString::FromUTF8(ragtime::picture::timeLabel(seconds_, seconds_)), &timeWide, &timeHigh);
        int most = std::max(1, (int)(width / std::max(1.0, timeWide * 2.0)));
        for (double at : ragtime::picture::timeTicks(seconds_, most))
        {
            double x = std::round(left + width * at / seconds_) + 0.5;
            gc->StrokeLine(x, top - tick, x, top);
            label(ragtime::picture::timeLabel(at, seconds_), x, 0.0);
        }
    }
    // Under it, the bytes.
    double labelWide = 0, high = 0;
    gc->GetTextExtent(wxString::FromUTF8(ragtime::picture::scaleLabel(whole_, whole_)), &labelWide, &high);
    int most = std::max(1, (int)(width / std::max(1.0, labelWide * 2.0)));
    for (uint64_t at : ragtime::picture::scaleTicks(whole_, most))
    {
        double x = std::round(xOf(at)) + 0.5;
        gc->StrokeLine(x, bar + 1.0, x, bar + tick);
        label(ragtime::picture::scaleLabel(at, whole_), x, bar + tick + 1.0);
    }
}
