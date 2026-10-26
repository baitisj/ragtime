#include "EarScope.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

#include "ragtime/ear/Passbands.h"
#include "ragtime/ear/Spectrum.h"

using ragtime::ear::Spectrum;

namespace
{

constexpr double RANGE_DB = 50.0;       // the waterfall's span above the floor
constexpr double BELOW_DB = 6.0;        // and below it
constexpr double PLOT_SPAN_DB = 70.0;   // the spectrum's height

// Night blue through blue and cyan to yellow, as in the mock-up.
void colourOf(double t, unsigned char* rgb)
{
    static const unsigned char stops[][3] = {{8, 12, 40}, {22, 52, 140}, {30, 150, 200}, {140, 220, 170}, {250, 235, 90}};
    t = std::max(0.0, std::min(1.0, t));
    const double k = t * 4.0;
    const int i = std::min(3, (int)k);
    const double f = k - i;
    for (int c = 0; c < 3; c++) rgb[c] = (unsigned char)std::lround(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f);
}

// The quieter part of a row: its 30th percentile.
double quietLevel(const std::vector<float>& row)
{
    std::vector<float> sorted(row);
    std::nth_element(sorted.begin(), sorted.begin() + (long)sorted.size() * 3 / 10, sorted.end());
    return sorted[sorted.size() * 3 / 10];
}

} // namespace

EarScope::EarScope(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
    , waterfall_(Spectrum::BINS, HISTORY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(520, 300)));
    clear();
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { mouseMove(e); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) { mouseDown(e); });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) { mouseUp(e); });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        grip_ = Grip::None;
        drag_.reset();
        Refresh();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        mouseX_ = -1;
        Refresh();
    });
}

void EarScope::clear()
{
    unsigned char rgb[3];
    colourOf(0.0, rgb);
    unsigned char* data = waterfall_.GetData();
    for (int i = 0; i < Spectrum::BINS * HISTORY; i++) std::memcpy(data + 3 * i, rgb, 3);
    recent_.clear();
    average_.clear();
    haveFloor_ = false;
    rows_ = 0;
    Refresh();
}

void EarScope::setRowsPerSecond(double rowsPerSecond)
{
    if (rowsPerSecond <= 0 || rowsPerSecond == rowsPerSecond_) return;
    rowsPerSecond_ = rowsPerSecond;
    clear();
}

void EarScope::setTopHz(double topHz)
{
    if (topHz <= 0 || topHz == topHz_) return;
    // Each bin of the new span takes the old bin under its frequency; past
    // the old top it is dark.
    const wxImage old = waterfall_.Copy();
    const unsigned char* from = old.GetData();
    unsigned char* to = waterfall_.GetData();
    unsigned char dark[3];
    colourOf(0.0, dark);
    for (int y = 0; y < HISTORY; y++)
        for (int i = 0; i < Spectrum::BINS; i++)
        {
            const double hz = Spectrum::binHz(i, topHz);
            unsigned char* px = to + 3 * (y * Spectrum::BINS + i);
            if (hz >= topHz_)
                std::memcpy(px, dark, 3);
            else
                std::memcpy(px, from + 3 * (y * Spectrum::BINS + Spectrum::binOf(hz, topHz_)), 3);
        }
    topHz_ = topHz;
    // The averaged spectrum starts afresh.
    recent_.clear();
    average_.clear();
    Refresh();
}

void EarScope::setNarrow(bool narrow)
{
    if (narrow == narrow_) return;
    narrow_ = narrow;
    Refresh();
}

void EarScope::setEdges(const std::optional<ragtime::ear::Edges>& edges)
{
    edges_ = edges;
    Refresh();
}

void EarScope::setRigWidth(int widthHz)
{
    if (widthHz == rigWidth_) return;
    rigWidth_ = widthHz;
    Refresh();
}

void EarScope::setHandles(const std::optional<ragtime::ear::Span>& at, bool together, bool shift)
{
    together_ = together;
    if (shift != shift_)
    {
        shift_ = shift;
        Refresh();
    }
    if (grip_ != Grip::None) return;     // not under the operator's hand
    handles_ = at;
    Refresh();
}

void EarScope::setWanted(const std::optional<ragtime::ear::Span>& wanted)
{
    wanted_ = wanted;
    Refresh();
}

double EarScope::hzAt(int x) const
{
    return std::max(0.0, std::min(topHz_, (x - plot_.x) * topHz_ / std::max(1, plot_.width)));
}

EarScope::Grip EarScope::gripAt(int x, int y) const
{
    if (!handles_ || y < plot_.y || y > plot_.GetBottom()) return Grip::None;
    auto xOf = [this](double hz) { return plot_.x + hz / topHz_ * plot_.width; };
    const int near = FromDIP(6);
    if (std::abs(x - (int)xOf(handles_->lowHz)) <= near) return Grip::Low;
    if (std::abs(x - (int)xOf(handles_->highHz)) <= near) return Grip::High;
    // The middle, while the rig has both PBT knobs.
    if (shift_ && x > xOf(handles_->lowHz) && x < xOf(handles_->highHz) && y <= plot_.y + FromDIP(30))
        return Grip::Shift;
    return Grip::None;
}

void EarScope::mouseDown(wxMouseEvent& e)
{
    grip_ = gripAt(e.GetX(), e.GetY());
    if (grip_ == Grip::None) return;
    gripHz_ = hzAt(e.GetX());
    dragFrom_ = *handles_;
    drag_ = dragFrom_;
    CaptureMouse();
    Refresh();
}

void EarScope::mouseMove(wxMouseEvent& e)
{
    mouseX_ = e.GetX();
    if (grip_ != Grip::None && drag_)
    {
        const double hz = hzAt(e.GetX());
        const double centre = dragFrom_.centre();
        ragtime::ear::Span s = dragFrom_;
        switch (grip_)
        {
        case Grip::Low:
            s.lowHz = std::min(hz, s.highHz - 50);
            if (together_) s.highHz = std::max(centre + 25, 2 * centre - s.lowHz);
            break;
        case Grip::High:
            s.highHz = std::max(hz, s.lowHz + 50);
            if (together_) s.lowHz = std::min(centre - 25, 2 * centre - s.highHz);
            break;
        case Grip::Shift:
            s.lowHz += hz - gripHz_;
            s.highHz += hz - gripHz_;
            break;
        case Grip::None: break;
        }
        drag_ = s;
    }
    else
    {
        const Grip g = gripAt(e.GetX(), e.GetY());
        SetCursor(g == Grip::Low || g == Grip::High ? wxCursor(wxCURSOR_SIZEWE)
                  : g == Grip::Shift                ? wxCursor(wxCURSOR_HAND)
                                                    : wxNullCursor);
    }
    Refresh();
}

void EarScope::mouseUp(wxMouseEvent&)
{
    if (grip_ == Grip::None) return;
    if (HasCapture()) ReleaseMouse();
    const std::optional<ragtime::ear::Span> s = drag_;
    grip_ = Grip::None;
    drag_.reset();
    Refresh();
    if (s && (std::fabs(s->lowHz - dragFrom_.lowHz) >= 10 || std::fabs(s->highHz - dragFrom_.highHz) >= 10) &&
        dragged_)
        dragged_(dragFrom_, *s);
}

void EarScope::addRows(const std::vector<std::vector<float>>& rows, bool transmitting)
{
    if (rows.empty()) return;
    for (const std::vector<float>& row : rows)
    {
        if ((int)row.size() != Spectrum::BINS) continue;
        // The floor follows slowly, and not while transmitting.
        if (!transmitting)
        {
            const double quiet = quietLevel(row);
            floor_ = haveFloor_ ? floor_ + (quiet - floor_) * 0.05 : quiet;
            haveFloor_ = true;
        }
        scroll(row, transmitting);
        if (!transmitting)
        {
            recent_.push_back(row);
            while ((double)recent_.size() > std::max(1.0, rowsPerSecond_)) recent_.pop_front();
        }
    }
    // The spectrum: the last second, averaged as power.
    if (!recent_.empty())
    {
        std::vector<double> power(Spectrum::BINS, 0.0);
        for (const auto& r : recent_)
            for (int i = 0; i < Spectrum::BINS; i++) power[i] += std::pow(10.0, r[i] / 10.0);
        average_.assign(Spectrum::BINS, 0.0f);
        for (int i = 0; i < Spectrum::BINS; i++)
            average_[i] = (float)(10.0 * std::log10(std::max(power[i] / (double)recent_.size(), 1e-16)));
    }
    Refresh();
}

void EarScope::scroll(const std::vector<float>& row, bool transmitting)
{
    unsigned char* data = waterfall_.GetData();
    const int stride = Spectrum::BINS * 3;
    std::memmove(data + stride, data, (std::size_t)stride * (HISTORY - 1));
    for (int i = 0; i < Spectrum::BINS; i++)
    {
        unsigned char* px = data + 3 * i;
        colourOf((row[i] - (floor_ - BELOW_DB)) / (RANGE_DB + BELOW_DB), px);
        if (transmitting)
        {
            const int grey = (px[0] + px[1] + px[2]) / 3;
            px[0] = px[1] = px[2] = (unsigned char)(40 + grey / 3);
        }
    }
    rows_++;
}

void EarScope::layoutAreas(int width, int height)
{
    wxClientDC dc(this);
    dc.SetFont(GetFont().Smaller());
    left_ = dc.GetTextExtent("-100 dB").x + FromDIP(8);
    const int right = FromDIP(10);
    const int axis = dc.GetCharHeight() + FromDIP(6);
    const int plotHeight = std::max(FromDIP(80), height * 3 / 10);
    plot_ = wxRect(left_, FromDIP(6), std::max(10, width - left_ - right), plotHeight);
    fall_ = wxRect(left_, plot_.GetBottom() + axis, plot_.width, std::max(10, height - plot_.GetBottom() - axis - 2));
}

void EarScope::paint()
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    layoutAreas(size.x, size.y);
    const wxColour panel = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const bool dark = panel.GetLuminance() < 0.5;
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    auto xOf = [this](double hz) { return plot_.x + hz / topHz_ * plot_.width; };
    const double bottomDb = (haveFloor_ ? floor_ : -100.0) - 10.0;
    auto yOf = [&](double db) {
        const double t = (db - bottomDb) / PLOT_SPAN_DB;
        return plot_.GetBottom() - std::max(0.0, std::min(1.0, t)) * plot_.height;
    };

    // The spectrum's ground, the passbands, the grid.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(panel));
    gc->DrawRectangle(plot_.x, plot_.y, plot_.width, plot_.height);
    const ragtime::ear::Passband wide = ragtime::ear::data2gPassband(false);
    const ragtime::ear::Passband narrow = ragtime::ear::data2gPassband(true);
    const wxColour green = dark ? wxColour(90, 170, 110) : wxColour(70, 150, 90);
    gc->SetBrush(wxBrush(wxColour(green.Red(), green.Green(), green.Blue(), narrow_ ? 25 : 55)));
    gc->DrawRectangle(xOf(wide.lowHz), plot_.y, xOf(wide.highHz) - xOf(wide.lowHz), plot_.height);
    gc->SetBrush(wxBrush(wxColour(green.Red(), green.Green(), green.Blue(), narrow_ ? 110 : 60)));
    gc->DrawRectangle(xOf(narrow.lowHz), plot_.y, xOf(narrow.highHz) - xOf(narrow.lowHz), plot_.height);

    wxFont small = GetFont().Smaller();
    gc->SetFont(small, text);
    const wxColour grid = dark ? wxColour(255, 255, 255, 40) : wxColour(0, 0, 0, 35);
    gc->SetPen(wxPen(grid));
    // A line every 500 Hz to 4.5 kHz, then every 1 or 2 kHz.
    const int gridHz = topHz_ <= 4500 ? 500 : topHz_ <= 8500 ? 1000 : 2000;
    for (int hz = gridHz; hz < (int)topHz_; hz += gridHz)
        gc->StrokeLine(xOf(hz), plot_.y, xOf(hz), plot_.GetBottom());
    for (int db = (int)std::ceil(bottomDb / 10) * 10; db <= bottomDb + PLOT_SPAN_DB; db += 10)
    {
        gc->StrokeLine(plot_.x, yOf(db), plot_.GetRight(), yOf(db));
        if ((db / 10) % 2 == 0)
        {
            wxString label = wxString::Format("%d dB", db);
            double w, h;
            gc->GetTextExtent(label, &w, &h);
            gc->DrawText(label, plot_.x - w - FromDIP(4), yOf(db) - h / 2);
        }
    }
    {
        wxString label = narrow_ ? wxString::FromUTF8("Data2G 500 Hz \xc2\xb7 1250\xe2\x80\x93" "1750")
                                 : wxString::FromUTF8("Data2G 2300 Hz \xc2\xb7 350\xe2\x80\x93" "2650");
        gc->SetFont(small, green.ChangeLightness(dark ? 130 : 70));
        gc->DrawText(label, xOf(narrow_ ? narrow.lowHz : wide.lowHz) + FromDIP(4), plot_.y + FromDIP(2));
        gc->SetFont(small, text);
    }

    // The averaged spectrum.
    if (!average_.empty())
    {
        wxGraphicsPath line = gc->CreatePath();
        for (int i = 0; i < Spectrum::BINS; i++)
        {
            const double x = xOf(Spectrum::binHz(i, topHz_)), y = yOf(average_[i]);
            if (i == 0) line.MoveToPoint(x, y);
            else line.AddLineToPoint(x, y);
        }
        gc->SetPen(wxPen(dark ? wxColour(120, 180, 255) : wxColour(28, 113, 216), FromDIP(2)));
        gc->StrokePath(line);
    }

    // The edges the noise shows: orange, with the -6 dB line between.
    const wxColour orange = dark ? wxColour(255, 160, 70) : wxColour(214, 100, 20);
    if (edges_)
    {
        gc->SetPen(wxPen(orange, FromDIP(2)));
        const double y = yOf(edges_->passbandDb - ragtime::ear::EdgeFinder::DROP_DB);
        for (const std::optional<double>& hz : {edges_->lowHz, edges_->highHz})
            if (hz) gc->StrokeLine(xOf(*hz), plot_.y, xOf(*hz), plot_.GetBottom());
        const double from = edges_->lowHz ? xOf(*edges_->lowHz) : plot_.x;
        const double to = edges_->highHz ? xOf(*edges_->highHz) : plot_.GetRight();
        wxPen dotted(orange, 1, wxPENSTYLE_SHORT_DASH);
        gc->SetPen(dotted);
        gc->StrokeLine(from, y, to, y);
        wxString label = wxString::FromUTF8("\xe2\x88\x92" "6 dB");
        if (edges_->lowHz && edges_->highHz)
            label += wxString::FromUTF8(" \xc2\xb7 ") +
                     // The width of the edges as the Ear's words give them, to 10 Hz.
                     wxString::Format("%d Hz", (int)(std::lround(*edges_->highHz / 10.0) -
                                                     std::lround(*edges_->lowHz / 10.0)) * 10);
        gc->SetFont(small, orange);
        double w, h;
        gc->GetTextExtent(label, &w, &h);
        gc->DrawText(label, std::max((double)plot_.x, (from + to - w) / 2), std::min(y + 2, plot_.GetBottom() - h));
        gc->SetFont(small, text);
    }
    // The rig's width, a dashed bracket over the top.
    if (rigWidth_ > 0)
    {
        const wxColour purple = dark ? wxColour(190, 150, 255) : wxColour(120, 70, 190);
        double centre = ragtime::ear::DATA2G_CENTRE_HZ;
        if (edges_ && edges_->lowHz && edges_->highHz) centre = (*edges_->lowHz + *edges_->highHz) / 2;
        const double x0 = xOf(centre - rigWidth_ / 2.0), x1 = xOf(centre + rigWidth_ / 2.0);
        const double y = plot_.y + FromDIP(18);
        gc->SetPen(wxPen(purple, FromDIP(2), wxPENSTYLE_SHORT_DASH));
        wxGraphicsPath bracket = gc->CreatePath();
        bracket.MoveToPoint(x0, y + FromDIP(6));
        bracket.AddLineToPoint(x0, y);
        bracket.AddLineToPoint(x1, y);
        bracket.AddLineToPoint(x1, y + FromDIP(6));
        gc->StrokePath(bracket);
        wxString label = wxString::Format("rig %d Hz", rigWidth_);
        gc->SetFont(small, purple);
        double w, h;
        gc->GetTextExtent(label, &w, &h);
        // Above the bracket's right end, clear of the edges' lines.
        gc->DrawText(label, std::max((double)plot_.x, std::min(x1, (double)plot_.GetRight()) - w), y - h - 1);
        gc->SetFont(small, text);
    }

    // The passband wanted (being dragged, or waiting, or on trial), dashed,
    // and what of Data2G's it would cut, in red.
    const std::optional<ragtime::ear::Span> wanted = drag_ ? drag_ : wanted_;
    if (wanted)
    {
        const ragtime::ear::Passband pb = narrow_ ? narrow : wide;
        const wxColour red = dark ? wxColour(255, 90, 80, 110) : wxColour(220, 30, 30, 90);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(red));
        const double stripY = plot_.GetBottom() - FromDIP(8);
        if (wanted->lowHz > pb.lowHz + 10)
            gc->DrawRectangle(xOf(pb.lowHz), stripY, xOf(std::min(wanted->lowHz, pb.highHz)) - xOf(pb.lowHz),
                              FromDIP(8));
        if (wanted->highHz < pb.highHz - 10)
        {
            const double from = std::max(wanted->highHz, pb.lowHz);
            gc->DrawRectangle(xOf(from), stripY, xOf(pb.highHz) - xOf(from), FromDIP(8));
        }
        const wxColour ink = dark ? wxColour(255, 255, 255) : wxColour(20, 20, 20);
        gc->SetPen(wxPen(ink, FromDIP(2), wxPENSTYLE_LONG_DASH));
        for (double hz : {wanted->lowHz, wanted->highHz}) gc->StrokeLine(xOf(hz), plot_.y, xOf(hz), plot_.GetBottom());
        if (drag_)
        {
            wxString label = wxString::Format("%d", (int)std::lround(wanted->lowHz / 10) * 10) +
                             wxString::FromUTF8("\xe2\x80\x93") +
                             wxString::Format("%d Hz", (int)std::lround(wanted->highHz / 10) * 10);
            double w, h;
            gc->SetFont(small, ink);
            gc->GetTextExtent(label, &w, &h);
            const double x = std::max((double)plot_.x, std::min(xOf(wanted->centre()) - w / 2, plot_.GetRight() - w));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(panel));
            gc->DrawRectangle(x - 2, plot_.y + FromDIP(34), w + 4, h + 2);
            gc->DrawText(label, x, plot_.y + FromDIP(35));
            gc->SetFont(small, text);
        }
    }
    // The handles: a tab on each edge, and the shift band between them.
    if (handles_ && !drag_)
    {
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(orange));
        const double tab = FromDIP(7);
        for (double hz : {handles_->lowHz, handles_->highHz})
        {
            wxGraphicsPath p = gc->CreatePath();
            p.MoveToPoint(xOf(hz) - tab, plot_.y);
            p.AddLineToPoint(xOf(hz) + tab, plot_.y);
            p.AddLineToPoint(xOf(hz), plot_.y + tab * 1.4);
            p.CloseSubpath();
            gc->FillPath(p);
        }
        if (shift_)
        {
            // The shift band, with a grip in its middle.
            const double l = xOf(handles_->lowHz), h = xOf(handles_->highHz), mid = (l + h) / 2;
            gc->SetBrush(wxBrush(wxColour(orange.Red(), orange.Green(), orange.Blue(), 40)));
            gc->DrawRectangle(l, plot_.y, h - l, FromDIP(30));
            // An orange pill with an arrow each way.
            const double g = FromDIP(4), y = plot_.y + FromDIP(15), pw = FromDIP(30), ph = FromDIP(16);
            gc->SetBrush(wxBrush(orange));
            gc->DrawRoundedRectangle(mid - pw / 2, y - ph / 2, pw, ph, ph / 2);
            gc->SetBrush(*wxWHITE_BRUSH);
            for (double side : {-1.0, 1.0})
            {
                wxGraphicsPath p = gc->CreatePath();
                p.MoveToPoint(mid + side * 2, y - g);
                p.AddLineToPoint(mid + side * (2 * g + 2), y);
                p.AddLineToPoint(mid + side * 2, y + g);
                p.CloseSubpath();
                gc->FillPath(p);
            }
        }
    }

    // The frequency axis between the two.
    for (int hz = 0; hz <= (int)topHz_; hz += gridHz)
    {
        const bool last = hz + gridHz > (int)topHz_;
        wxString label = hz == 0 ? wxString("0") : last ? wxString::Format("%d Hz", hz) : wxString::Format("%d", hz);
        double w, h;
        gc->GetTextExtent(label, &w, &h);
        double x = std::min(xOf(hz) - w / 2, (double)plot_.GetRight() - w);
        gc->DrawText(label, std::max((double)plot_.x, x), plot_.GetBottom() + FromDIP(3));
    }

    // The waterfall, scaled to fit, and its time labels.
    const int shown = std::min(HISTORY, fall_.height);
    wxImage part = waterfall_.GetSubImage(wxRect(0, 0, Spectrum::BINS, shown));
    part.Rescale(fall_.width, fall_.height, wxIMAGE_QUALITY_NORMAL);
    gc->DrawBitmap(wxBitmap(part), fall_.x, fall_.y, fall_.width, fall_.height);
    const double secondsPerPixel = (double)shown / fall_.height / rowsPerSecond_;
    const int every = secondsPerPixel * fall_.height > 60 ? 20 : 10;
    for (int s = 0; s * 1.0 / secondsPerPixel < fall_.height; s += every)
    {
        const double y = fall_.y + s / secondsPerPixel;
        wxString label = s == 0 ? wxString("now") : wxString::FromUTF8("\xe2\x88\x92") + wxString::Format("%d s", s);
        double w, h;
        gc->GetTextExtent(label, &w, &h);
        gc->DrawText(label, fall_.x - w - FromDIP(4), std::min(y, (double)fall_.GetBottom() - h));
    }
    // The passband's edges, faintly down the waterfall, and the edges the
    // noise shows.
    gc->SetPen(wxPen(wxColour(255, 255, 255, 70)));
    const ragtime::ear::Passband in = narrow_ ? narrow : wide;
    for (double hz : {in.lowHz, in.highHz}) gc->StrokeLine(xOf(hz), fall_.y, xOf(hz), fall_.GetBottom());
    if (edges_)
    {
        gc->SetPen(wxPen(wxColour(orange.Red(), orange.Green(), orange.Blue(), 150), 1, wxPENSTYLE_SHORT_DASH));
        for (const std::optional<double>& hz : {edges_->lowHz, edges_->highHz})
            if (hz) gc->StrokeLine(xOf(*hz), fall_.y, xOf(*hz), fall_.GetBottom());
    }

    // The frequency under the mouse.
    if (mouseX_ >= plot_.x && mouseX_ <= plot_.GetRight())
    {
        const double hz = (mouseX_ - plot_.x) * topHz_ / plot_.width;
        gc->SetPen(wxPen(dark ? wxColour(255, 255, 255, 140) : wxColour(0, 0, 0, 120)));
        gc->StrokeLine(mouseX_, plot_.y, mouseX_, fall_.GetBottom());
        wxString label = wxString::Format("%d Hz", (int)std::lround(hz / 10.0) * 10);
        double w, h;
        gc->GetTextExtent(label, &w, &h);
        double x = mouseX_ + FromDIP(5);
        if (x + w > plot_.GetRight()) x = mouseX_ - FromDIP(5) - w;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(panel));
        gc->DrawRectangle(x - 2, plot_.y + plot_.height / 2.0 - 1, w + 4, h + 2);
        gc->DrawText(label, x, plot_.y + plot_.height / 2.0);
    }
}
