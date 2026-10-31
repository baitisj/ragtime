// Save as WAV: a voice message's sound as a plain WAV file, for keeping
// outside Ragtime or playing anywhere. Ragtime itself keeps the .sot file,
// 3 KB for 10 s against 160 KB of WAV.
#ifndef RAGTIME_VOICE_WAV_H
#define RAGTIME_VOICE_WAV_H

#include <cstdint>
#include <vector>

namespace ragtime
{
namespace voice
{

// A 16-bit mono PCM WAV file of samples at rate (sotto's 8 kHz by
// default).
std::vector<uint8_t> wavFile(const std::vector<int16_t>& samples, int rate = 8000);

} // namespace voice
} // namespace ragtime

#endif
