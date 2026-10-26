#ifndef RAGTIME_EAR_FRAME_H
#define RAGTIME_EAR_FRAME_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "ragtime/audio/EarAudio.h"
#include "ragtime/ear/Edges.h"
#include "ragtime/ear/Spectrum.h"
#include "ragtime/radio/RigFilter.h"

class EarScope;
class wxChoice;
class wxStaticText;

// The Ear (docs/STAGE1.md, "The Ear"): a floating window with a waterfall
// of what the radio hears, opened by the ear button on the connection bar.
// It listens alongside data2g-host (audio::EarAudio) to the sound card the
// host records from, or to a source picked in "Listen to", and only while
// it is open: closing it stops the recording.
//
// Under the controls it says what the rig reports of its receive filter
// (read between bursts while the Ear is open, through rigctld or Hamlib)
// and the edges the band's noise shows (ear::EdgeFinder), and the scope
// draws both: step 2 of docs/STAGE1.md's "The Ear", read only.
//
// "Listen to" and the speed are kept in wxConfig (keys::EAR_SOURCE,
// keys::EAR_SPEED). The window tells the owner when it closes.
// What the rig says of its filter, for the Ear.
struct EarRig
{
    std::string via;    // "rigctld", "Hamlib"; empty with no rig chosen
    std::optional<ragtime::radio::RigFilter> filter;
    // The band's noise from the S-meter, in dB over S9 (the gearshift's QRN).
    std::optional<double> noiseDb;
};

class EarFrame : public wxFrame
{
public:
    EarFrame(wxWindow* parent, std::function<void()> onClosed);
    ~EarFrame() override;

    // data2g-host's address and command port, to find its process when it
    // runs on this machine (several hosts are told apart that way).
    void setHost(const std::string& host, int commandPort);
    // The 500 Hz switch, for which passband is darker.
    void setNarrow(bool narrow);
    // data2g-host's PTT: rows heard while it is on are greyed.
    void setTransmitting(bool on);
    // Asked twice a second what the rig says of its filter.
    void setRig(std::function<EarRig()> rig);

private:
    void tick();
    void showStatus(const ragtime::audio::EarStatus& status);
    void chooseSource();
    void lookForHost();
    void showFilter(const ragtime::audio::EarStatus& status);

    std::unique_ptr<ragtime::audio::EarAudio> audio_;
    std::function<void()> onClosed_;

    wxChoice* source_ = nullptr;
    wxChoice* speed_ = nullptr;
    wxStaticText* format_ = nullptr;
    EarScope* scope_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxStaticText* rigSays_ = nullptr;
    wxStaticText* noiseShows_ = nullptr;
    wxTimer timer_;

    // Filled on PipeWire's thread, drained by the timer.
    std::mutex mutex_;
    ragtime::ear::Spectrum spectrum_;
    std::vector<std::vector<float>> pending_;
    std::atomic<bool> changed_{true};
    ragtime::ear::EdgeFinder edges_;
    std::function<EarRig()> rig_;
    wxString noiseText_;    // " · QRN S3", beside the rate

    std::vector<std::string> sourceNames_;  // beside source_'s items; "" follows the host
    std::string chosen_;
    std::string host_;
    int commandPort_ = 0;
    bool transmitting_ = false;
    int ticks_ = 0;
};

#endif
