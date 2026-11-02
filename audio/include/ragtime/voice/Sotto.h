// libsotto, Jeff's 2,400 bit/s vocoder, loaded at run time (docs/STAGE1.md,
// "Voice messages"). sotto keeps Codec 2's analysis and synthesis, which
// are LGPL-2.1, so it is a shared library (libsotto.so.0, sotto.dll) that
// Ragtime opens with dlopen or LoadLibrary, as it opens PipeWire, and never
// links: Ragtime stays MIT, and runs without it, saying voice is missing.
//
// Where it looks, in order: the file named by RAGTIME_SOTTO; beside the
// program (the Windows zip's sotto.dll, the AppImage's usr/lib); then the
// system's library path. A library is taken only if its sotto_format() is
// sotto v3.8 (voice::codecOfFormat), which codes codec 2 from 16 kHz speech
// and plays codecs 1 and 2, so a libsotto built as another variant or with
// another codebook is passed over rather than sending frames nobody can
// play.
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

// sotto.h's figures, so callers need no sotto.h. Ragtime's voice is 16 kHz
// both ways (sotto v3.8's sotto_encode16 and sotto_decode16); a codec 1
// clip is decoded at 8 kHz and doubled.
constexpr int SOTTO_RATE = 16000;
constexpr int SOTTO_SAMPLES = 320;      // 20 ms at 16 kHz, one frame
constexpr int SOTTO_FRAME_BYTES = 6;
// The 16 kHz decoder's sound comes this many frames behind
// (SOTTO_DEC16_DELAY_FRAMES); the 8 kHz one's, one.
constexpr int SOTTO_DEC16_DELAY = 5;
using SottoFrame = std::array<uint8_t, SOTTO_FRAME_BYTES>;

struct SottoApi;

// What loading found.
struct SottoInfo
{
    bool available = false;
    std::string format;     // sotto_format(): "sotto v3.8, shape 9-9-8-8"
    uint8_t codec = 0;      // the .sot codec number it sends (voice::codecOfFormat), 0 if none
    std::string path;       // the file loaded
    std::string why;        // not available: why, in a sentence for the operator
};

// Turns 16 kHz speech into codec 2's frames. Frames come about 320 ms
// behind (the pitch tracker's look-ahead, then v3.8's voicing rules'):
// flush() at the end gives the rest.
class SottoEncoder
{
public:
    ~SottoEncoder();
    SottoEncoder(const SottoEncoder&) = delete;
    SottoEncoder& operator=(const SottoEncoder&) = delete;

    // 320 samples (20 ms) in; true and a frame out once the look-ahead has
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

// Turns a clip's frames back into 16 kHz speech, delay() frames behind:
// the first calls give silence, and flush() gives what it still holds. A
// codec 2 clip goes through sotto_decode16, with its hiss band, 5 frames
// behind; a codec 1 clip through the 8 kHz sotto_decode (sotto_decode16
// would read its unvoiced frames' pitch as hiss), doubled to 16 kHz, one
// frame behind. A lost frame (good false) is covered as sotto covers one
// on 2 m: the last sound held 40 ms, fading 6 dB per 20 ms, silent after
// 80 ms.
class SottoDecoder
{
public:
    ~SottoDecoder();
    SottoDecoder(const SottoDecoder&) = delete;
    SottoDecoder& operator=(const SottoDecoder&) = delete;

    uint8_t codec() const { return codec_; }
    // Frames the sound comes behind.
    int delay() const { return codec_ == CODEC_V36 ? 1 : SOTTO_DEC16_DELAY; }

    // 320 samples out.
    void decode(const SottoFrame& frame, bool good, int16_t* samples);
    // 320 samples of what it still holds, one frame's a call; false once
    // it holds none.
    bool flush(int16_t* samples);

    // A clip's frames (good: one a frame, 0 for lost; empty for all good)
    // into its sound, the decoder's delay taken out, so sample 0 is the
    // clip's start. Uses a fresh decoder state.
    std::vector<int16_t> decodeClip(const std::vector<SottoFrame>& frames, const std::vector<uint8_t>& good);

private:
    friend class Sotto;
    static constexpr uint8_t CODEC_V36 = 1;
    struct Doubler;
    SottoDecoder(const SottoApi* api, uint8_t codec);
    void create();
    void destroy();
    const SottoApi* api_;
    uint8_t codec_;
    void* state_ = nullptr;
    std::unique_ptr<Doubler> doubler_;  // codec 1: 8 kHz to 16 kHz
    int held_ = 0;                      // frames fed whose sound hasn't come out
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

    // Null when the library isn't available. The encoder makes codec 2;
    // the decoder plays clips of codec (1 or 2, voice::Clip::codec), null
    // for any other.
    std::unique_ptr<SottoEncoder> encoder() const;
    std::unique_ptr<SottoDecoder> decoder(uint8_t codec) const;

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
