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
class wxChoice;
class wxSlider;
class wxSpinCtrl;
class wxStaticText;

// A gear a picture can be planned for: the dialog's list, the first the
// one it starts with (Jeff, 2026-10-27: from the path, and the operator
// can pick another).
struct PictureGear
{
    std::string label;              // in the list: "E2C, from how we hear KC2G (−3.1 dB, 4 bursts)"
    std::string words;              // in the sentences: "gear E2C"
    ragtime::link::ModeInfo mode;   // what it is planned for
    std::string sendMode;           // on D2G, the mode it goes in ("" for the host's own)
};

// Where a picture goes, and what the dialog needs to say how long it takes.
struct PictureTarget
{
    bool broadcast = true;          // to D2G, else in the session with peer
    std::string peer;
    std::string callsign;           // ours
    std::vector<PictureGear> gears; // empty when the host has listed none
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
    // The gear picked: on D2G, the mode the picture goes in ("" for the
    // host's own).
    const PictureGear& gear() const { return gear_; }

private:
    void choose();
    bool load(const std::string& path);
    // The longest side or the time moved: again shortly after.
    void later();
    void remake();
    // The preview playing the picture arriving: a step, the next, and how
    // long the one on screen stays at the speed picked.
    void showStep();
    void nextStep();
    int stepMs() const;
    void showSpeed();
    // Who sees it: the peer, or "a listener" on D2G.
    wxString farEnd(bool capital = false) const;
    // A blank the picture's size, for a step with nothing to show yet.
    wxImage blank() const;
    uint64_t budget(double seconds) const;
    double airSeconds(uint64_t bytes) const;

    PictureTarget target_;
    PictureGear gear_;
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
    wxChoice* gears_ = nullptr;
    wxStaticText* size_ = nullptr;
    wxStaticText* air_ = nullptr;
    wxStaticText* seal_ = nullptr;
    wxSlider* speed_ = nullptr;         // 2 to the power of it: 1× to 64×
    wxStaticText* speedText_ = nullptr;
    std::vector<ragtime::picture::ArrivalStep> steps_;
    std::vector<wxImage> frames_;       // each step decoded, when first shown
    size_t step_ = 0;
    static constexpr int MAX_SPEED_STEP = 6;
    static constexpr int DEFAULT_SPEED_STEP = 3;   // 8×
    static constexpr int MIN_STEP_MS = 80;
    static constexpr int MAX_STEP_MS = 5000;
    static constexpr int HOLD_MS = 3000;
    wxStaticText* sizeText_ = nullptr;
    wxStaticText* partialText_ = nullptr;
    wxButton* send_ = nullptr;
    wxTimer timer_;
    wxTimer play_;
};

#endif
