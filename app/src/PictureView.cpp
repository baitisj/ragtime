#include "PictureView.h"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include "Html.h"

PictureView::PictureView(wxWindow* parent, const wxSize& minSize, bool zooming)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, minSize, wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE)
    , zooming_(zooming)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(minSize);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        if (fit_ && onZoom_ && image_.IsOk()) onZoom_(scale());
        clampCentre();
        Refresh();
        e.Skip();
    });
    if (!zooming_) return;
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        if (!image_.IsOk() || e.GetWheelDelta() == 0) return;
        double steps = (double)e.GetWheelRotation() / e.GetWheelDelta();
        zoomAbout(scale() * std::pow(1.25, steps), wxPoint2DDouble(e.GetPosition().x, e.GetPosition().y));
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        if (!image_.IsOk()) return;
        if (fit_)
        {
            scale_ = fitScale();
            fit_ = false;
        }
        dragging_ = true;
        dragFrom_ = e.GetPosition();
        centreFrom_ = centre_;
        CaptureMouse();
    });
    auto stop = [this](wxMouseEvent&) {
        if (!dragging_) return;
        dragging_ = false;
        if (HasCapture()) ReleaseMouse();
    };
    Bind(wxEVT_LEFT_UP, stop);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragging_ = false; });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!dragging_) return;
        wxPoint moved = e.GetPosition() - dragFrom_;
        centre_ = wxPoint2DDouble(centreFrom_.m_x - moved.x / scale_, centreFrom_.m_y - moved.y / scale_);
        clampCentre();
        Refresh();
    });
    // A double click goes between fitting and a pixel to a pixel.
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
        if (!image_.IsOk()) return;
        if (fit_ || std::fabs(scale_ - 1.0) > 1e-6)
            zoomAbout(1.0, wxPoint2DDouble(e.GetPosition().x, e.GetPosition().y));
        else
            fit();
    });
}

void PictureView::setImage(const wxImage& image, bool keepView)
{
    bool sameSize = image_.IsOk() && image.IsOk() && image.GetWidth() == image_.GetWidth()
                    && image.GetHeight() == image_.GetHeight();
    image_ = image;
    haveBitmap_ = false;
    if (image_.IsOk())
    {
        bitmap_ = wxGraphicsRenderer::GetDefaultRenderer()->CreateBitmapFromImage(image_);
        haveBitmap_ = !bitmap_.IsNull();
    }
    if (!keepView || !sameSize)
    {
        fit_ = true;
        centre_ = wxPoint2DDouble(image_.GetWidth() / 2.0, image_.GetHeight() / 2.0);
        if (onZoom_ && image_.IsOk()) onZoom_(scale());
    }
    Refresh();
}

void PictureView::setNote(const wxString& note)
{
    note_ = note;
    Refresh();
}

double PictureView::fitScale() const
{
    if (!image_.IsOk()) return 1.0;
    wxSize room = GetClientSize();
    double s = std::min((double)room.x / image_.GetWidth(), (double)room.y / image_.GetHeight());
    // A small picture shows at most four screen pixels to its one.
    return std::max(MIN_SCALE, std::min(s, 4.0));
}

double PictureView::scale() const
{
    return fit_ ? fitScale() : scale_;
}

void PictureView::fit()
{
    fit_ = true;
    centre_ = wxPoint2DDouble(image_.GetWidth() / 2.0, image_.GetHeight() / 2.0);
    if (onZoom_ && image_.IsOk()) onZoom_(scale());
    Refresh();
}

void PictureView::zoomTo(double s)
{
    wxSize room = GetClientSize();
    zoomAbout(s, wxPoint2DDouble(room.x / 2.0, room.y / 2.0));
}

void PictureView::zoomAbout(double s, wxPoint2DDouble at)
{
    if (!image_.IsOk()) return;
    s = std::max(MIN_SCALE, std::min(MAX_SCALE, s));
    double old = scale();
    wxSize room = GetClientSize();
    // The picture point under at stays under it.
    wxPoint2DDouble under(centre_.m_x + (at.m_x - room.x / 2.0) / old, centre_.m_y + (at.m_y - room.y / 2.0) / old);
    fit_ = false;
    scale_ = s;
    centre_ = wxPoint2DDouble(under.m_x - (at.m_x - room.x / 2.0) / s, under.m_y - (at.m_y - room.y / 2.0) / s);
    clampCentre();
    if (onZoom_) onZoom_(scale_);
    Refresh();
}

void PictureView::clampCentre()
{
    if (!image_.IsOk()) return;
    if (fit_)
    {
        centre_ = wxPoint2DDouble(image_.GetWidth() / 2.0, image_.GetHeight() / 2.0);
        return;
    }
    // A picture smaller than the window sits in its middle; a bigger one
    // can be moved until its edge meets the window's.
    wxSize room = GetClientSize();
    auto clamp = [](double c, double length, double view) {
        if (length <= view) return length / 2.0;
        return std::max(view / 2.0, std::min(length - view / 2.0, c));
    };
    centre_.m_x = clamp(centre_.m_x, image_.GetWidth(), room.x / scale_);
    centre_.m_y = clamp(centre_.m_y, image_.GetHeight(), room.y / scale_);
}

void PictureView::paint()
{
    wxAutoBufferedPaintDC dc(this);
    wxColour back = blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW),
                                wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT), 0.12);
    dc.SetBackground(wxBrush(back));
    dc.Clear();
    wxSize room = GetClientSize();
    if (!image_.IsOk() || !haveBitmap_)
    {
        if (note_.empty()) return;
        dc.SetTextForeground(secondaryText());
        dc.SetFont(GetFont());
        wxSize text = dc.GetTextExtent(note_);
        dc.DrawText(note_, (room.x - text.x) / 2, (room.y - text.y) / 2);
        return;
    }
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    double s = scale();
    // Close up, each picture pixel a sharp square; further out, smoothed.
    gc->SetInterpolationQuality(s >= 2.0 ? wxINTERPOLATION_NONE : wxINTERPOLATION_BEST);
    double w = image_.GetWidth() * s, h = image_.GetHeight() * s;
    double x = room.x / 2.0 - centre_.m_x * s, y = room.y / 2.0 - centre_.m_y * s;
    gc->DrawBitmap(bitmap_, std::round(x), std::round(y), w, h);
}
