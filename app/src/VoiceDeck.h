#ifndef RAGTIME_VOICE_DECK_H
#define RAGTIME_VOICE_DECK_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "ragtime/deck/Meters.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/voice/Chain.h"
#include "ragtime/voice/Sotto.h"

namespace ragtime
{
namespace audio
{
class VoiceAudio;
}
} // namespace ragtime

class AirtimeBar;
class DeckKnob;
class DeckLamp;
class DeckMeter;
class PitchScope;
class ReductionBar;
class TalkButton;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxSlider;
class wxStaticText;

// Where a clip can go now, and what it costs there, from the owner.
struct DeckTarget
{
    std::string callsign;               // ours
    // The session partner, when a session is up; empty for none.
    std::string peer;
    ragtime::link::ModeInfo sessionMode;    // what the session's data goes in (may be unknown)
    double sessionBytesPerSecond = 0.0;     // picture::sessionBytesPerSecond
    std::string sessionGear;                // "gear E2C"
    // On D2G: the mode in effect, and its words.
    ragtime::link::ModeInfo broadcastMode;
    std::string broadcastGear;
};

// What the deck asks of the main window.
struct VoiceDeckHooks
{
    // The program's one microphone and speaker (MainFrame::voiceAudio).
    std::function<ragtime::audio::VoiceAudio&()> audio;
    std::function<DeckTarget()> target;
    // Keeps a coded clip (a .sot file's bytes) and queues it to the
    // session partner or D2G. Returns the kept path, or "" and why not.
    std::function<std::string(const std::vector<uint8_t>& sot, bool broadcast, std::string& error)> send;
    // How a clip sent is getting on, by its kept path.
    std::function<ragtime::deck::ClipAir(const std::string& path)> clipAir;
    // While Push to talk is held, voice from others waits.
    std::function<void(bool hold)> holdPlayback;
    std::function<void()> onClosed;
};

// The microphone processor deck (docs/STAGE1.md, "The microphone deck"):
// a floating window, a rack front panel read left to right in the order
// the sound flows. Input (the microphone, two meters, Gain, Clip),
// Clean-up (High-pass, Noise), Leveller (Target, push-pull, and the gain
// reduction), sotto (the pitch trace and each clip's Level check), and To
// the air (where it goes, Push to talk, Monitor, the clip's lamps); the
// airtime bar along the foot, and the speaker and the review box under it.
//
// The microphone is open while the deck is, so the meters show the level
// before you talk. Hold Push to talk (or the space bar while the deck has
// focus) to record; let go and sotto codes the clip and it goes, as a
// voice file, to the session partner or the net. Esc while holding throws
// it away. Monitor plays the last clip through sotto's encoder and
// decoder, as the far end will hear it; it and Push to talk release each
// other. With "Hold my clips for review" ticked, letting go keeps the clip
// in the deck with Send and Discard.
//
// The deck never keys the rig: data2g-host keys it when the file's bursts
// go, as for text. The knobs are kept in wxConfig (keys::VOICE_DECK_*).
class VoiceDeck : public wxFrame
{
public:
    VoiceDeck(wxWindow* parent, VoiceDeckHooks hooks);
    ~VoiceDeck() override;

private:
    void buildPanel();
    void loadSettings();
    void saveSettings() const;
    void applySettings();
    void tick();
    void showDevices();
    void showTarget();
    void showMeters(double seconds);
    void showClipLamps();
    void showCodec();
    void showLevelCheck(const ragtime::voice::LevelReport& report, bool have);
    void updateAirtime();
    bool sendToBroadcast() const;

    // The microphone's samples, on the audio thread.
    void onSamples(const float* samples, size_t n, int rate);
    // Push to talk.
    void talkDown();
    void talkUp();
    void discardClip();
    void finishClip();
    void sendClip();
    void drainCoded();
    void toggleMonitor();
    void stopMonitor();

    void onCharHook(wxKeyEvent& e);
    void onKeyUp(wxKeyEvent& e);
    // Space for Push to talk, unless a standard control has focus.
    bool spaceIsTalk() const;
    void bindKeyUp(wxWindow* w);

    VoiceDeckHooks hooks_;
    ragtime::voice::Sotto& sotto_;
    wxTimer timer_;
    // Survives the window for callbacks that come after it has gone.
    std::shared_ptr<bool> alive_;

    // The panel.
    wxStaticText* where_ = nullptr;         // "Session with KC2G"
    wxStaticText* gear_ = nullptr;          // "GEAR E2C · w48-qpsk-r1/2 · 1,611 bit/s"
    wxChoice* microphone_ = nullptr;
    DeckMeter* active_ = nullptr;
    DeckMeter* loudest_ = nullptr;
    wxStaticText* activeText_ = nullptr;
    wxStaticText* loudestText_ = nullptr;
    DeckKnob* gain_ = nullptr;
    DeckLamp* clip_ = nullptr;
    DeckKnob* highPass_ = nullptr;
    DeckLamp* highPassLamp_ = nullptr;
    DeckKnob* noise_ = nullptr;
    DeckLamp* noiseLamp_ = nullptr;
    DeckKnob* target_ = nullptr;
    ReductionBar* reduction_ = nullptr;
    PitchScope* scope_ = nullptr;
    wxStaticText* codec_ = nullptr;
    DeckLamp* checkActive_ = nullptr;
    DeckLamp* checkLoudest_ = nullptr;
    DeckLamp* checkNoise_ = nullptr;
    wxStaticText* checkActiveText_ = nullptr;
    wxStaticText* checkLoudestText_ = nullptr;
    wxStaticText* checkNoiseText_ = nullptr;
    wxChoice* sendTo_ = nullptr;
    TalkButton* talk_ = nullptr;
    wxButton* monitor_ = nullptr;
    wxButton* reviewSend_ = nullptr;
    wxButton* reviewDiscard_ = nullptr;
    DeckLamp* recLamp_ = nullptr;
    DeckLamp* codingLamp_ = nullptr;
    DeckLamp* queuedLamp_ = nullptr;
    DeckLamp* sendingLamp_ = nullptr;
    AirtimeBar* airtime_ = nullptr;
    wxStaticText* airWords_ = nullptr;
    wxStaticText* airGear_ = nullptr;
    wxChoice* speaker_ = nullptr;
    wxSlider* volume_ = nullptr;
    wxCheckBox* review_ = nullptr;
    wxStaticText* status_ = nullptr;

    // What the knobs say, handed to the chain.
    ragtime::voice::ChainSettings settings_;

    // Filled on the audio thread, under mutex_.
    std::mutex mutex_;
    std::unique_ptr<ragtime::voice::Chain> chain_;
    bool recording_ = false;
    std::vector<float> coded8k_;            // 8 kHz, waiting for the encoder
    std::vector<float> scratch_;            // the audio thread's own
    bool newSettings_ = true;

    // The window's.
    ragtime::deck::Talk talk_state_;
    ragtime::deck::VuNeedle activeNeedle_{ragtime::deck::ACTIVE_FLOOR_DBFS};
    ragtime::deck::PeakNeedle loudestNeedle_{ragtime::deck::LOUDEST_FLOOR_DBFS};
    int clipTicks_ = 0;                     // the Clip lamp stays lit a while
    std::unique_ptr<ragtime::voice::SottoEncoder> encoder_;
    std::vector<ragtime::voice::SottoFrame> frames_;   // the clip being made
    uint64_t recordStartMs_ = 0;
    uint64_t codingSinceMs_ = 0;
    bool spaceHeld_ = false;
    // The last clip: its frames (for Monitor), its file, and where it went.
    std::vector<ragtime::voice::SottoFrame> lastFrames_;
    std::vector<uint8_t> lastSot_;
    std::string lastPath_;
    ragtime::deck::ClipAir lastAir_ = ragtime::deck::ClipAir::None;
    DeckTarget targetNow_;
    std::vector<std::string> micNames_, speakerNames_;
    int ticks_ = 0;
};

#endif
