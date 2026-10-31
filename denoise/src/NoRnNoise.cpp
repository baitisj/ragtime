// The stand-in for builds with -DRAGTIME_RNNOISE=OFF: no noise reduction.
#include "ragtime/denoise/RnNoise.h"

namespace ragtime
{
namespace denoise
{

bool haveRnNoise()
{
    return false;
}

std::unique_ptr<voice::Denoiser> makeRnNoise()
{
    return nullptr;
}

} // namespace denoise
} // namespace ragtime
