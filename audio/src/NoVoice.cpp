// VoiceAudio's stand-in where it has nothing to record or play through:
// builds with -DRAGTIME_PIPEWIRE=OFF, and the Mac.
#include "ragtime/audio/VoiceAudio.h"

namespace ragtime
{
namespace audio
{

namespace
{

const char* const WHY = "Voice records and plays through PipeWire on Linux and WASAPI on Windows; this build has neither";

class NoVoice : public VoiceAudio
{
public:
    void start(Changed changed, const std::vector<std::string>&) override
    {
        if (changed) changed();
    }
    void stop() override {}
    void chooseMicrophone(const std::string&) override {}
    void chooseSpeaker(const std::string&) override {}
    void forgetRadio() override {}
    void listen(Samples) override {}
    void stopListening() override {}
    void play(std::vector<float>, int, Finished finished) override
    {
        if (finished) finished(false);
    }
    void playStream(int, Finished finished) override
    {
        if (finished) finished(false);
    }
    void append(const float*, std::size_t) override {}
    void endStream() override {}
    void stopPlaying() override {}
    double playPosition() const override { return 0; }
    void setVolume(float) override {}
    VoiceStatus status() const override
    {
        VoiceStatus s;
        s.problem = WHY;
        return s;
    }
};

} // namespace

std::unique_ptr<VoiceAudio> makeVoiceAudio()
{
    return std::make_unique<NoVoice>();
}

} // namespace audio
} // namespace ragtime
