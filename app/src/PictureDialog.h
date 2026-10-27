#ifndef RAGTIME_PICTURE_DIALOG_H
#define RAGTIME_PICTURE_DIALOG_H

#include <cstdint>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/timer.h>

#include "ragtime/image/Codec.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/picture/Picture.h"

class PictureView;
class wxButton;
class wxSlider;
class wxSpinCtrl;
class wxStaticText;

// Where a picture goes, and what the dialog needs to say how long it takes.
struct PictureTarget
{
    bool broadcast = true;          // to D2G, else in the session with peer
    std::string peer;
    std::string callsign;           // ours
    ragtime::link::ModeInfo mode;   // the gear it goes in; not valid() when none is known
    std::string gear;               // "gear 3, E2C", for the words
    double measuredRate = 0.0;      // a session's acknowledged bytes a second, 0 when not yet measured
};

// Send a picture (docs/STAGE1.md, "Inline pictures"): the picture picked,
// shrunk with the detail-preserving downscaler to fit the longest side
// asked for and coded as progressive JPEG 2000 to fit the airtime asked
// for, shown as the far end will see it, whole or as it comes. On a
// broadcast the seal after it is airtime too, said on a line of its own.
// Send is greyed when not even a small picture fits the time in the gear:
// "too slow".
class PictureDialog : public wxDialog
{
public:
    PictureDialog(wxWindow* parent, const PictureTarget& target, const std::string& path);

    // After Send: the codestream, and its name ("shack.j2k").
    const std::vector<uint8_t>& coded() const { return coded_.bytes; }
    const std::string& name() const { return name_; }

private:
    void choose();
    bool load(const std::string& path);
    // The longest side or the time moved: again shortly after.
    void later();
    void remake();
    void showPartial();
    uint64_t budget(double seconds) const;
    double airSeconds(uint64_t bytes) const;

    PictureTarget target_;
    std::string path_;
    std::string name_;
    ragtime::picture::Image picked_;    // as read, turned the way up
    ragtime::picture::Image shrunk_;    // at the longest side asked for
    int shrunkSide_ = 0;
    ragtime::image::Coded coded_;
    wxImage whole_;                     // the coded picture, decoded

    PictureView* view_ = nullptr;
    wxStaticText* file_ = nullptr;
    wxSpinCtrl* side_ = nullptr;
    wxSpinCtrl* seconds_ = nullptr;
    wxStaticText* size_ = nullptr;
    wxStaticText* air_ = nullptr;
    wxStaticText* seal_ = nullptr;
    wxSlider* partial_ = nullptr;
    wxStaticText* partialText_ = nullptr;
    wxButton* send_ = nullptr;
    wxTimer timer_;
};

#endif
