// libsotto, Jeff's vocoder, loaded at run time (docs/STAGE1.md, "Voice
// messages"). sotto keeps Codec 2's analysis and synthesis, which
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
// play. The slower rates come with what the library exports: sotto 1200
// (codec 3) with sotto_lr_*, sotto 600 and 400 (codecs 4 and 5) with
// sotto_vlr_*, sent only when it can decode them too (a library with the
// 600 and 400 encoder but not yet their decoder sends neither).
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

#include "ragtime/voice/Voice.h"

namespace ragtime
{
namespace voice
{

// sotto.h's figures, so callers need no sotto.h. Ragtime's voice is 16 kHz
// both ways (sotto v3.8's sotto_encode16 and sotto_decode16); the 8 kHz
// codecs (1, 3, 4 and 5) are halved to code and decoded at 8 kHz and
// doubled.
constexpr int SOTTO_RATE = 16000;
constexpr int SOTTO_SAMPLES = 320;      // 20 ms at 16 kHz, one 2,400 frame
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
    uint8_t codec = 0;      // the 2,400 codec it sends (voice::codecOfFormat), 0 if none
    std::vector<uint8_t> sends;     // the codecs it codes and plays, fastest first
    std::vector<uint8_t> plays;     // the codecs it plays
    std::string path;       // the file loaded
    std::string why;        // not available: why, in a sentence for the operator
};

// Turns 16 kHz speech into a codec's frames. Codec 2's come about 320 ms
// behind (the pitch tracker's look-ahead, then v3.8's voicing rules');
// the slower codecs' speech is halved to 8 kHz first, and their frames come
// a few frames behind: flush() at the end gives the rest.
class SottoEncoder
{
public:
    ~SottoEncoder();
    SottoEncoder(const SottoEncoder&) = delete;
    SottoEncoder& operator=(const SottoEncoder&) = delete;

    uint8_t codec() const { return codec_; }
    // Any number of 16 kHz samples: whole blocks go to sotto as they fill,
    // the rest waits for the next call. Frames made are appended to out.
    void push(const int16_t* samples, size_t count, std::vector<SottoFrame>& out);
    // Ends the clip: a last part block is padded with silence, and the
    // frames still in the look-ahead are appended to out. The encoder is
    // spent after this.
    void flush(std::vector<SottoFrame>& out);

private:
    friend class Sotto;
    struct Halver;
    SottoEncoder(const SottoApi* api, uint8_t codec, void* state);
    // A block of the coded rate's samples to sotto: 320 at 16 kHz for
    // codec 2, 320 at 8 kHz for 1,200, 80 at 8 kHz for 600 and 400.
    void block(const int16_t* samples, std::vector<SottoFrame>& out);
    void coded(const int16_t* samples, size_t count, std::vector<SottoFrame>& out);
    size_t blockSamples() const;
    const SottoApi* api_;
    uint8_t codec_;
    void* state_;
    std::unique_ptr<Halver> halver_;    // the 8 kHz codecs: 16 kHz to 8 kHz
    std::vector<int16_t> part_;         // at the coded rate
    bool flushed_ = false;
};

// Turns a clip's frames back into 16 kHz speech, frameSamples() a frame,
// delay() frames behind: the first calls give silence, and flush() gives
// what it still holds. A codec 2 clip goes through sotto_decode16, with
// its hiss band, 5 frames behind; a codec 1 clip through the 8 kHz
// sotto_decode (sotto_decode16 would read its unvoiced frames' pitch as
// hiss), doubled to 16 kHz, one frame behind. The slower codecs' decoders
// play a frame once they have the two after it: sotto 1200 two frames
// behind, 600 and 400 three (v3.7's core adds 20 ms, the part frame made
// up with silence at the start). A lost frame (good false) is covered as sotto covers one on 2 m:
// the last sound held 40 ms, fading 6 dB per 20 ms, silent after 80 ms;
// the slower codecs rebuild a lone lost frame from both sides.
class SottoDecoder
{
public:
    ~SottoDecoder();
    SottoDecoder(const SottoDecoder&) = delete;
    SottoDecoder& operator=(const SottoDecoder&) = delete;

    uint8_t codec() const { return codec_; }
    // Frames the sound comes behind.
    int delay() const { return delay_; }
    // 16 kHz samples a frame: 320 at 2,400, up to 1,920 at 400.
    size_t frameSamples() const { return frameSamples_; }

    // frameSamples() out.
    void decode(const SottoFrame& frame, bool good, int16_t* samples);
    // frameSamples() of what it still holds, one frame's a call; false
    // once it holds none.
    bool flush(int16_t* samples);

    // A clip's frames (good: one a frame, 0 for lost; empty for all good)
    // into its sound, the decoder's delay taken out, so sample 0 is the
    // clip's start. Uses a fresh decoder state.
    std::vector<int16_t> decodeClip(const std::vector<SottoFrame>& frames, const std::vector<uint8_t>& good);

private:
    friend class Sotto;
    struct Doubler;
    SottoDecoder(const SottoApi* api, uint8_t codec);
    void create();
    void destroy();
    // The coded rate's sound of one frame into fifo_, at 16 kHz.
    void take(const int16_t* raw, size_t count);
    // frameSamples() off the front of fifo_, silence first if it is short.
    void give(int16_t* samples);
    const SottoApi* api_;
    uint8_t codec_;
    int delay_ = 1;
    size_t frameSamples_ = SOTTO_SAMPLES;
    void* state_ = nullptr;
    std::unique_ptr<Doubler> doubler_;  // the 8 kHz codecs: 8 kHz to 16 kHz
    std::vector<int16_t> fifo_;         // sound at 16 kHz not yet given
    bool drained_ = false;              // sotto's own flush has given all it had
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

    // Whether it codes (and plays) codec, and plays it.
    bool sends(uint8_t codec) const;
    bool plays(uint8_t codec) const;

    // Null when the library isn't available or doesn't do the codec. The
    // encoder makes codec (2 by default, any of info().sends); the decoder
    // plays clips of codec (voice::Clip::codec, any of info().plays).
    std::unique_ptr<SottoEncoder> encoder(uint8_t codec = CODEC_SEND) const;
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
