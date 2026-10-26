#ifndef RAGTIME_EAR_FRAME_H
#define RAGTIME_EAR_FRAME_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "ragtime/audio/EarAudio.h"
#include "ragtime/ear/Spectrum.h"

class EarScope;
class wxChoice;
class wxStaticText;

// The Ear (docs/STAGE1.md, "The Ear"): a floating window with a waterfall
// of what the radio hears, opened by the ear button on the connection bar.
// It listens alongside data2g-host (audio::EarAudio) to the sound card the
// host records from, or to a source picked in "Listen to", and only while
// it is open: closing it stops the recording.
//
// "Listen to" and the speed are kept in wxConfig (keys::EAR_SOURCE,
// keys::EAR_SPEED). The window tells the owner when it closes.
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

private:
    void tick();
    void showStatus(const ragtime::audio::EarStatus& status);
    void chooseSource();
    void lookForHost();

    std::unique_ptr<ragtime::audio::EarAudio> audio_;
    std::function<void()> onClosed_;

    wxChoice* source_ = nullptr;
    wxChoice* speed_ = nullptr;
    wxStaticText* format_ = nullptr;
    EarScope* scope_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxTimer timer_;

    // Filled on PipeWire's thread, drained by the timer.
    std::mutex mutex_;
    ragtime::ear::Spectrum spectrum_;
    std::vector<std::vector<float>> pending_;
    std::atomic<bool> changed_{true};

    std::vector<std::string> sourceNames_;  // beside source_'s items; "" follows the host
    std::string chosen_;
    std::string host_;
    int commandPort_ = 0;
    bool transmitting_ = false;
    int ticks_ = 0;
};

#endif
