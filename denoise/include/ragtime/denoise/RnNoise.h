// Noise reduction for the voice chain (docs/STAGE1.md, "Voice: the
// processing chain"): RNNoise, Jean-Marc Valin's recurrent network noise
// suppressor (Xiph.Org, BSD-3-Clause), as a voice::Denoiser.
//
// Kept out of ragtime_core, which links no libraries. RNNoise is built
// from its git repository at 0.1.1, the last release whose trained model
// is in the repository itself (0.2's is downloaded from media.xiph.org at
// build time). With -DRAGTIME_RNNOISE=OFF the stand-in makes none, and the
// chain's Noise stage passes the voice through.
#ifndef RAGTIME_DENOISE_RNNOISE_H
#define RAGTIME_DENOISE_RNNOISE_H

#include <memory>

#include "ragtime/voice/Chain.h"

namespace ragtime
{
namespace denoise
{

// Whether this build has RNNoise.
bool haveRnNoise();

// A new RNNoise denoiser (10 ms frames at 48 kHz), or null without one.
std::unique_ptr<voice::Denoiser> makeRnNoise();

} // namespace denoise
} // namespace ragtime

#endif
