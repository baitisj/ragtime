#ifndef RAGTIME_PICTURE_DIALOG_H
#define RAGTIME_PICTURE_DIALOG_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/timer.h>

#include "ragtime/image/Codec.h"
#include "ragtime/image/CoolChic.h"
#include "ragtime/image/CoolChicEncode.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/picture/Picture.h"

class ArrivalBar;
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
// asked for and coded to fit the airtime asked for, shown as the far end
// will see it, whole or as it comes. On a broadcast the seal after it is
// airtime too, said on a line of its own. Send is greyed when not even a
// small picture fits the time in the gear: "too slow".
//
// Two codecs: progressive JPEG 2000, made at once, or Cool-chic
// (docs/PICTURE_CODEC.md), sharper for the bytes but fitted to the picture
// over a few minutes on a thread of its own. While it fits, the preview
// shows the picture as the fit has it, the bar its progress (filling from
// the right, over a percent scale), and Cancel stops it after asking;
// then the preview plays the picture arriving, as for JPEG 2000.
class PictureDialog : public wxDialog
{
public:
    PictureDialog(wxWindow* parent, const PictureTarget& target, const std::string& path);
    ~PictureDialog() override;

    // After Send: the codestream or Cool-chic stream, and its name
    // ("shack.j2k", "shack.rcc").
    const std::vector<uint8_t>& coded() const { return coded_.bytes; }
    const std::string& name() const { return sentAs_; }
    // The gear picked: on D2G, the mode the picture goes in ("" for the
    // host's own).
    const PictureGear& gear() const { return gear_; }

private:
    enum Codec
    {
        JPEG2000 = 0,
        COOLCHIC = 1
    };
    // A Cool-chic fit on its thread: what it hands the dialog.
    struct Fit
    {
        int side = 0;
        uint64_t budget = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::atomic<double> done{0.0};
        std::mutex lock;                    // over the rest
        ragtime::picture::Image preview;    // the latest picture from the fit
        bool fresh = false;                 // not shown yet
        bool ok = false;
        ragtime::image::CoolChicResult result;
        std::string error;
    };

    void choose();
    bool load(const std::string& path);
    // The longest side or the time moved: again shortly after.
    void later();
    void remake();
    // JPEG 2000 in budget bytes, at once; false, with the line said, if
    // it can't be made.
    bool makeJpeg2000(uint64_t budget);
    // Cool-chic in budget bytes: from a fit made before, or a fit started.
    // False until there is one.
    bool makeCoolChic(uint64_t budget);
    void startFit(uint64_t budget);
    // The fit's thread is told to stop; it goes on finishing in the
    // background and is joined later.
    void stopFit();
    // The fit's progress and picture, shown; its end, taken.
    void watchFit();
    bool fitting() const { return fit_ != nullptr; }
    // Asks before throwing a fit away (Jeff, 2026-10-29); true to stop.
    bool askStop(bool closing);
    void holdSettings(bool hold);
    // The coded picture, whole: the lines under it, and the preview
    // playing it arriving.
    void showCoded();
    void showFitting(double done);
    Codec codec() const;
    // What the picture is sent as: name_ with the codec's extension.
    std::string sentName() const;
    // The preview playing the picture arriving: a step, the next, and how
    // long the one on screen stays at the speed picked.
    void showStep();
    void nextStep();
    int stepMs() const;
    // Dragged on the bar: the step nearest that many bytes.
    void seek(uint64_t bytes);
    void showSpeed();
    // The gear's label and list in red while the gear is too slow.
    void showGearTooSlow(bool slow);
    // The picture torn about a third of the way across, the rest gone:
    // what a gear too slow would get through.
    wxImage torn(const wxImage& picture) const;
    // A blank the picture's size, for a step with nothing to show yet.
    wxImage blank(int width, int height) const;
    // "about 52 s on the air in gear E2C".
    wxString onTheAir(uint64_t bytes) const;
    uint64_t budget(double seconds) const;
    double airSeconds(uint64_t bytes) const;

    PictureTarget target_;
    PictureGear gear_;
    std::string path_;
    std::string name_;
    std::string sentAs_;                // name_, with the codec's extension, at Send
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
    wxChoice* codec_ = nullptr;
    wxButton* another_ = nullptr;
    wxButton* stop_ = nullptr;          // under the bar: Cancel the fit, or Encode again
    wxStaticText* gearLabel_ = nullptr;
    wxStaticText* sideLabel_ = nullptr;     // lit with the bar's largest figures
    wxStaticText* airLabel_ = nullptr;
    wxStaticText* air_ = nullptr;
    wxStaticText* seal_ = nullptr;
    wxSlider* speed_ = nullptr;         // 2 to the power of it: 1× to 64×
    wxStaticText* speedText_ = nullptr;
    std::vector<ragtime::picture::ArrivalStep> steps_;
    std::vector<wxImage> frames_;       // each step decoded, when first shown
    size_t step_ = 0;
    bool paused_ = false;               // the pointer over the bar
    static constexpr int MAX_SPEED_STEP = 6;
    static constexpr int DEFAULT_SPEED_STEP = 3;   // 8×
    static constexpr int MIN_STEP_MS = 80;
    static constexpr int MAX_STEP_MS = 5000;
    static constexpr int HOLD_MS = 3000;
    ArrivalBar* arrival_ = nullptr;
    wxStaticText* sizeText_ = nullptr;
    wxButton* send_ = nullptr;
    wxTimer timer_;
    wxTimer play_;

    // Cool-chic: the fit going, those told to stop and not yet joined,
    // the streams made for this picture (by side and budget), and a fit
    // stopped for these settings, which waits for Encode or a change.
    std::unique_ptr<Fit> fit_;
    std::vector<std::unique_ptr<Fit>> stopped_;
    std::map<std::pair<int, uint64_t>, ragtime::image::CoolChicResult> fits_;
    bool cancelled_ = false;
    wxString fitError_;                 // why the last fit couldn't be made
    // The preview's Cool-chic decoder, fed step by step.
    std::unique_ptr<ragtime::image::coolchic::Decoder> stepper_;
    uint64_t stepperBytes_ = 0;
    uint64_t fitStartMs_ = 0;
    wxTimer watch_;
    static constexpr int WATCH_MS = 100;
};

#endif
