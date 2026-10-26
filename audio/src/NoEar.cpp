// EarAudio's stand-in where there is no PipeWire to load: Windows, and
// builds with -DRAGTIME_PIPEWIRE=OFF.
#include "ragtime/audio/EarAudio.h"

namespace ragtime
{
namespace audio
{

namespace
{

class NoEar : public EarAudio
{
public:
    void start(Samples, Changed changed) override
    {
        if (changed) changed();
    }
    void stop() override {}
    void choose(const std::string&) override {}
    void setHostPid(std::optional<int>) override {}
    EarStatus status() const override
    {
        EarStatus s;
        s.problem = "The Ear listens through PipeWire, on Linux";
        return s;
    }
};

} // namespace

std::unique_ptr<EarAudio> makeEarAudio()
{
    return std::make_unique<NoEar>();
}

} // namespace audio
} // namespace ragtime
