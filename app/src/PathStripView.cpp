#include "PathStripView.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include "Html.h"
#include "ragtime/gearbox/Gearbox.h"

using ragtime::radio::BandRow;
using ragtime::radio::HourCell;
using ragtime::radio::PathStrip;

namespace
{

constexpr uint64_t HOUR_MS = 60ull * 60 * 1000;

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

std::string hourText(size_t hour)
{
    char text[8];
    std::snprintf(text, sizeof text, "%02u00", (unsigned)(hour % 24));
    return text;
}

} // namespace

PathStripView::PathStripView(wxWindow* parent, int cellDip)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
    , cellDip_(cellDip)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { onPaint(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { onMotion(e.GetPosition()); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { UnsetToolTip(); });
}

void PathStripView::setStrip(const PathStrip& strip, uint64_t nowMs)
{
    bool sameShape = strip.bands.size() == strip_.bands.size();
    strip_ = strip;
    nowMs_ = nowMs;
    if (!sameShape)
    {
        InvalidateBestSize();
        if (GetParent()) GetParent()->Layout();
    }
    Refresh();
}

int PathStripView::labelWidth() const
{
    wxClientDC dc(const_cast<PathStripView*>(this));
    dc.SetFont(GetFont());
    return dc.GetTextExtent("Dial ?").x + FromDIP(6);
}

int PathStripView::rowHeight() const
{
    return std::max(FromDIP(cellDip_), GetCharHeight() * 3 / 4);
}

int PathStripView::cellWidth() const
{
    int room = GetClientSize().x - labelWidth();
    return std::max(FromDIP(4), std::min(FromDIP(cellDip_ * 2), room / 24));
}

wxSize PathStripView::DoGetBestClientSize() const
{
    size_t rows = std::max<size_t>(1, strip_.bands.size());
    int height = (int)rows * (rowHeight() + FromDIP(2)) + GetCharHeight() + FromDIP(2);
    return wxSize(labelWidth() + 24 * FromDIP(cellDip_), height);
}

void PathStripView::onPaint()
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour surface = GetParent() ? GetParent()->GetBackgroundColour()
                                         : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    dc.SetBackground(wxBrush(surface));
    dc.Clear();
    dc.SetFont(GetFont());
    dc.SetTextForeground(secondaryText());

    if (strip_.bands.empty())
    {
        dc.DrawText(wxString::Format("Not in the path log for the last %u days", (unsigned)strip_.days), 0, 0);
        return;
    }

    const int left = labelWidth();
    const int cell = cellWidth();
    const int row = rowHeight();
    const int gap = FromDIP(2);
    const wxColour empty = blendColour(surface, ink, 0.08);
    const wxColour heard = accentText();
    int y = 0;
    for (const BandRow& band : strip_.bands)
    {
        wxString label = band.band.empty() ? wxString("Dial ?") : utf8(band.band);
        dc.SetTextForeground(ink);
        dc.DrawText(label, 0, y + (row - dc.GetCharHeight()) / 2);
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (size_t h = 0; h < 24; h++)
        {
            const HourCell& c = band.hours[h];
            wxColour fill = empty;
            if (c.bursts > 0)
            {
                // More of the days heard in this hour, the darker.
                double share = strip_.days > 0 ? (double)c.days / (double)strip_.days : 1.0;
                fill = blendColour(surface, heard, 0.35 + 0.65 * std::min(1.0, share * 3.0));
            }
            dc.SetBrush(wxBrush(fill));
            dc.DrawRectangle(left + (int)h * cell, y, std::max(1, cell - 1), row);
        }
        y += row + gap;
    }

    // The hours along the foot, and the hour now.
    dc.SetTextForeground(secondaryText());
    for (size_t h = 0; h < 24; h += 6)
    {
        wxString text = utf8(hourText(h).substr(0, 2));
        dc.DrawText(text, left + (int)h * cell, y);
    }
    wxString utc = "UTC";
    dc.DrawText(utc, left + 24 * cell - dc.GetTextExtent(utc).x, y);
    if (nowMs_ != 0)
    {
        size_t hour = (size_t)(nowMs_ / HOUR_MS % 24);
        int x = left + (int)hour * cell;
        dc.SetPen(wxPen(heard, FromDIP(2)));
        dc.DrawLine(x, 0, x, y - gap);
        dc.DrawLine(x + cell - 1, 0, x + cell - 1, y - gap);
    }
}

void PathStripView::onMotion(const wxPoint& at)
{
    const int left = labelWidth();
    const int cell = cellWidth();
    const int row = rowHeight() + FromDIP(2);
    wxString tip;
    if (at.x >= left && at.x < left + 24 * cell && row > 0)
    {
        size_t band = (size_t)(at.y / row);
        size_t hour = (size_t)((at.x - left) / cell);
        if (band < strip_.bands.size() && hour < 24)
        {
            const BandRow& b = strip_.bands[band];
            const HourCell& c = b.hours[hour];
            std::string text = (b.band.empty() ? std::string("Band not known") : b.band) + ", " + hourText(hour)
                               + "\xE2\x80\x93" + hourText(hour + 1) + "Z: ";
            if (c.bursts == 0)
            {
                text += "not heard";
            }
            else
            {
                text += std::to_string(c.bursts) + (c.bursts == 1 ? " burst" : " bursts") + " on "
                        + std::to_string(c.days) + (c.days == 1 ? " day" : " days");
                if (c.medianDb) text += ", median " + ragtime::gearbox::formatDb(*c.medianDb);
            }
            tip = utf8(text);
        }
    }
    if (GetToolTipText() != tip)
    {
        if (tip.empty())
            UnsetToolTip();
        else
            SetToolTip(tip);
    }
}
