// VoiceAudio: the microphone and speaker for voice messages, Ragtime's
// first (docs/STAGE1.md, "Voice audio"). PipeWire on Linux, WASAPI in
// shared mode on Windows, on the same foundations as the Ear (EarAudio.h).
//
// It never touches the radio's sound card: the devices it offers leave out
// every card data2g-host records from or plays to, both halves of it, and
// the ones it used before (voice::devices), and it refuses one picked by
// hand. With no pick it uses the system's default, or the first other
// device when the default is the radio's. Each stream is pinned to its
// device, so the session manager can never move it onto the radio.
//
// The microphone is open only between listen() and stopListening(), at
// the device's own rate, mono. play() plays one clip of mono floats at any
// rate (PipeWire or the Windows audio engine converts it to the device's)
// and replaces anything playing.
//
// Callbacks come on the audio thread (PipeWire's, or VoiceAudio's own on
// Windows): hand the samples on, don't block.
#ifndef RAGTIME_AUDIO_VOICE_AUDIO_H
#define RAGTIME_AUDIO_VOICE_AUDIO_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ragtime/voice/Devices.h"

namespace ragtime
{
namespace audio
{

struct VoiceStatus
{
    // Connected to PipeWire (or WASAPI); when not, problem says why.
    bool available = false;
    std::string problem;
    // What the dropdowns offer, and the radio's cards left out of them.
    voice::Devices devices;
    // The picks ("" for the default) and what they come to.
    std::string chosenMicrophone;
    std::string chosenSpeaker;
    voice::DevicePick microphone;
    voice::DevicePick speaker;
    // The microphone: open (listen), samples coming, its rate, and why
    // it couldn't open.
    bool listening = false;
    bool recording = false;
    int microphoneRate = 0;
    std::string microphoneProblem;
    // The speaker: a clip playing, and why it couldn't play.
    bool playing = false;
    std::string speakerProblem;
};

class VoiceAudio
{
public:
    using Samples = std::function<void(const float* samples, std::size_t n, int rate)>;
    using Changed = std::function<void()>;
    using Finished = std::function<void(bool played)>;

    virtual ~VoiceAudio() = default;

    // Connects to PipeWire (or WASAPI) and watches the devices; changed
    // is called when status() may have changed. remembered: the radio
    // cards' names from before (voice::rememberRadio).
    virtual void start(Changed changed, const std::vector<std::string>& remembered) = 0;
    // Disconnects; no callbacks after it returns.
    virtual void stop() = 0;
    // "" for the system's default; otherwise a device's name.
    virtual void chooseMicrophone(const std::string& name) = 0;
    virtual void chooseSpeaker(const std::string& name) = 0;
    // Forgets the radio cards remembered from before (those in use now
    // stay left out).
    virtual void forgetRadio() = 0;

    // Opens the microphone; samples get mono floats at its own rate. It
    // follows a new pick, and reopens when the device comes back.
    virtual void listen(Samples samples) = 0;
    virtual void stopListening() = 0;

    // Plays a clip of mono floats at any rate on the speaker picked,
    // replacing anything playing. finished(true) once it has all been
    // heard, finished(false) if it was stopped, replaced or couldn't play.
    virtual void play(std::vector<float> samples, int rate, Finished finished) = 0;
    // A clip that plays as it comes (voice arriving in a ragchew): it
    // starts empty, replacing anything playing; append() adds to it and it
    // plays on with no gap; while nothing new has come the speaker plays
    // silence. endStream() says nothing more is coming, and finished(true)
    // follows once it has all been heard. append and endStream do nothing
    // once the clip has been stopped or replaced.
    virtual void playStream(int rate, Finished finished) = 0;
    virtual void append(const float* samples, std::size_t n) = 0;
    virtual void endStream() = 0;
    virtual void stopPlaying() = 0;
    // How far into the clip playing the speaker has been handed, in
    // seconds (a buffer, 100 ms or so, ahead of the ear): for a playhead.
    virtual double playPosition() const = 0;
    // The speaker's volume, applied at once, the clip playing included:
    // a gain, 1 as it is (keys::VOICE_VOLUME holds it in dB).
    virtual void setVolume(float gain) = 0;

    virtual VoiceStatus status() const = 0;
};

// PipeWire on Linux, WASAPI on Windows; a stand-in that says why not
// elsewhere.
std::unique_ptr<VoiceAudio> makeVoiceAudio();

} // namespace audio
} // namespace ragtime

#endif
