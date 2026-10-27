#ifndef RAGTIME_PICTURE_VIEW_H
#define RAGTIME_PICTURE_VIEW_H

#include <functional>
#include <memory>

#include <wx/geometry.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/panel.h>

// A picture drawn to fit its window, or at a zoom: the send dialog's
// preview and the picture window's view. With zooming on, the wheel zooms
// about the pointer and a drag moves the picture about.
class PictureView : public wxPanel
{
public:
    PictureView(wxWindow* parent, const wxSize& minSize, bool zooming);

    // A new picture, or the same one sharper: kept where it is unless
    // keepView is false.
    void setImage(const wxImage& image, bool keepView = true);
    // A line in the middle while there is no picture ("Reading...").
    void setNote(const wxString& note);

    // The scale on the screen: 1 is a picture pixel to a screen pixel.
    double scale() const;
    // Fits the picture to the window, as it starts.
    void fit();
    bool fitting() const { return fit_; }
    // A scale, about the window's middle.
    void zoomTo(double scale);
    static constexpr double MIN_SCALE = 0.05;
    static constexpr double MAX_SCALE = 16.0;
    // Told when the scale moves (the wheel, fit, a resize while fitting).
    void onZoom(std::function<void(double scale)> fn) { onZoom_ = std::move(fn); }

private:
    void paint();
    double fitScale() const;
    void zoomAbout(double scale, wxPoint2DDouble at);
    void clampCentre();

    wxImage image_;
    wxGraphicsBitmap bitmap_;
    bool haveBitmap_ = false;
    wxString note_;
    bool zooming_ = false;
    bool fit_ = true;
    double scale_ = 1.0;
    wxPoint2DDouble centre_;        // the picture point at the window's middle
    bool dragging_ = false;
    wxPoint dragFrom_;
    wxPoint2DDouble centreFrom_;
    std::function<void(double)> onZoom_;
};

#endif
