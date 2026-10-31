#include "ragtime/denoise/RnNoise.h"

#include <algorithm>

#include "rnnoise.h"

namespace ragtime
{
namespace denoise
{

namespace
{
class RnNoise : public voice::Denoiser
{
public:
    RnNoise()
        : state_(rnnoise_create(nullptr))
    {
    }
    ~RnNoise() override
    {
        if (state_)
            rnnoise_destroy(state_);
    }
    RnNoise(const RnNoise&) = delete;
    RnNoise& operator=(const RnNoise&) = delete;

    bool ok() const { return state_ != nullptr && rnnoise_get_frame_size() == FRAME; }

    float process(float* frame) override
    {
        // RNNoise works in 16-bit sample units.
        float in[FRAME], out[FRAME];
        for (int i = 0; i < FRAME; i++)
            in[i] = frame[i] * 32768.0f;
        float voice = rnnoise_process_frame(state_, out, in);
        for (int i = 0; i < FRAME; i++)
            frame[i] = out[i] / 32768.0f;
        return std::clamp(voice, 0.0f, 1.0f);
    }

    // RNNoise's overlap-add puts its output one frame behind its input.
    int latency() const override { return FRAME; }

    void reset() override
    {
        if (state_)
            rnnoise_destroy(state_);
        state_ = rnnoise_create(nullptr);
    }

    const char* name() const override { return "RNNoise 0.1.1"; }

private:
    DenoiseState* state_;
};
} // namespace

bool haveRnNoise()
{
    return true;
}

std::unique_ptr<voice::Denoiser> makeRnNoise()
{
    auto d = std::make_unique<RnNoise>();
    if (!d->ok())
        return nullptr;
    return d;
}

} // namespace denoise
} // namespace ragtime
