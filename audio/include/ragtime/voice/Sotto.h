// libsotto, Jeff's 2,400 bit/s vocoder, loaded at run time (docs/STAGE1.md,
// "Voice messages"). sotto keeps Codec 2's analysis and synthesis, which
// are LGPL-2.1, so it is a shared library (libsotto.so.0, sotto.dll) that
// Ragtime opens with dlopen or LoadLibrary, as it opens PipeWire, and never
// links: Ragtime stays MIT, and runs without it, saying voice is missing.
//
// Where it looks, in order: the file named by RAGTIME_SOTTO; beside the
// program (the Windows zip's sotto.dll, the AppImage's usr/lib); then the
// system's library path. A library is taken only if its sotto_format() is
// a codec Ragtime knows (voice::codecOfFormat), so a libsotto built with
// another codebook is refused rather than sending frames nobody can play.
//
// One library for the whole program; encoders and decoders are cheap and
// each belongs to one thread at a time.
#ifndef RAGTIME_VOICE_SOTTO_H
#define RAGTIME_VOICE_SOTTO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ragtime
{
namespace voice
{

// sotto.h's figures, so callers need no sotto.h.
constexpr int SOTTO_RATE = 8000;
constexpr int SOTTO_SAMPLES = 160;      // 20 ms at 8 kHz, one frame
constexpr int SOTTO_FRAME_BYTES = 6;
using SottoFrame = std::array<uint8_t, SOTTO_FRAME_BYTES>;

struct SottoApi;

// What loading found.
struct SottoInfo
{
    bool available = false;
    std::string format;     // sotto_format(): "sotto v3.6, shape 9-9-8-8"
    uint8_t codec = 0;      // the .sot codec number it speaks (voice::codecOfFormat), 0 if none
    std::string path;       // the file loaded
    std::string why;        // not available: why, in a sentence for the operator
};

// Turns 8 kHz speech into frames. Frames come 80 ms behind (the pitch
// tracker's look-ahead): flush() at the end gives the rest.
class SottoEncoder
{
public:
    ~SottoEncoder();
    SottoEncoder(const SottoEncoder&) = delete;
    SottoEncoder& operator=(const SottoEncoder&) = delete;

    // 160 samples (20 ms) in; true and a frame out once the look-ahead has
    // filled.
    bool encode(const int16_t* samples, SottoFrame& out);
    // Any number of samples: whole frames go to encode() as they fill, the
    // rest waits for the next call. Frames made are appended to out.
    void push(const int16_t* samples, size_t count, std::vector<SottoFrame>& out);
    // Ends the clip: a last part frame is padded with silence, and the
    // frames still in the look-ahead are appended to out. The encoder is
    // spent after this.
    void flush(std::vector<SottoFrame>& out);

private:
    friend class Sotto;
    SottoEncoder(const SottoApi* api, void* state) : api_(api), state_(state) {}
    const SottoApi* api_;
    void* state_;
    std::vector<int16_t> part_;
    bool flushed_ = false;
};

// Turns frames back into 8 kHz speech, 20 ms behind: the first call gives
// silence, and flush() gives the last 20 ms. A lost frame (good false) is
// covered as sotto covers one on 2 m: the last sound held 40 ms, fading
// 6 dB per 20 ms, silent after 80 ms.
class SottoDecoder
{
public:
    ~SottoDecoder();
    SottoDecoder(const SottoDecoder&) = delete;
    SottoDecoder& operator=(const SottoDecoder&) = delete;

    // 160 samples out.
    void decode(const SottoFrame& frame, bool good, int16_t* samples);
    void flush(int16_t* samples);

    // A clip's frames (good: one a frame, 0 for lost; empty for all good)
    // into its sound, the decoder's 20 ms delay taken out, so sample 0 is
    // the clip's start. Uses a fresh decoder state.
    std::vector<int16_t> decodeClip(const std::vector<SottoFrame>& frames, const std::vector<uint8_t>& good);

private:
    friend class Sotto;
    SottoDecoder(const SottoApi* api, void* state) : api_(api), state_(state) {}
    const SottoApi* api_;
    void* state_;
};

class Sotto
{
public:
    // The program's library, loaded the first time this is asked.
    static Sotto& library();
    // A library from one file only, for tests ("" looks where library()
    // looks).
    explicit Sotto(const std::string& path);
    ~Sotto();
    Sotto(const Sotto&) = delete;
    Sotto& operator=(const Sotto&) = delete;

    const SottoInfo& info() const { return info_; }
    bool available() const { return info_.available; }

    // Null when the library isn't available.
    std::unique_ptr<SottoEncoder> encoder() const;
    std::unique_ptr<SottoDecoder> decoder() const;

private:
    bool open(const std::string& file, std::string& why);
    void close();

    void* handle_ = nullptr;
    std::unique_ptr<SottoApi> api_;
    SottoInfo info_;
};

// Where library() looks, in order, for tests and the About box.
std::vector<std::string> sottoCandidates();

// 16-bit samples and floats (1.0 = full scale) both ways.
std::vector<float> toFloat(const std::vector<int16_t>& samples);

} // namespace voice
} // namespace ragtime

#endif
