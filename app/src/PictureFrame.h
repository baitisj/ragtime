#ifndef RAGTIME_PICTURE_FRAME_H
#define RAGTIME_PICTURE_FRAME_H

#include <cstdint>
#include <functional>
#include <string>

#include <wx/frame.h>
#include <wx/image.h>

class PictureView;
class wxSlider;
class wxStaticText;

// A picture among the messages, clicked: a window of its own with a zoom
// control (Jeff, 2026-10-27). Fit, 1:1 and the slider set the zoom; the
// wheel zooms about the pointer and a drag moves the picture. A picture
// still coming sharpens in it as more arrives. Save as... keeps it as a
// PNG, as it stands.
class PictureFrame : public wxFrame
{
public:
    PictureFrame(wxWindow* parent, const wxString& title, const std::string& suggestedName);

    // The picture as it stands: decoded from bytes of size in all.
    void show(const wxImage& image, int width, int height, uint64_t bytes, uint64_t size);
    // Told when the window closes.
    void onClosed(std::function<void()> fn) { onClosed_ = std::move(fn); }

private:
    void zoomed(double scale);
    void save();

    PictureView* view_ = nullptr;
    wxSlider* zoom_ = nullptr;
    wxStaticText* zoomText_ = nullptr;
    wxStaticText* about_ = nullptr;
    wxImage image_;
    std::string suggestedName_;
    std::function<void()> onClosed_;
};

#endif
