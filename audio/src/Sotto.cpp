// libsotto loaded at run time: see Sotto.h.
#include "ragtime/voice/Sotto.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ragtime/voice/Dsp.h"
#include "ragtime/voice/Voice.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace ragtime
{
namespace voice
{

// sotto.h's functions, as pointers.
struct SottoApi
{
    const char* (*format)() = nullptr;
    void* (*encCreate)() = nullptr;
    void (*encDestroy)(void*) = nullptr;
    int (*encode)(void*, const short*, unsigned char*) = nullptr;
    void (*encodeHalf)(void*, const short*) = nullptr;
    int (*encFlush)(void*, unsigned char*) = nullptr;
    void* (*decCreate)() = nullptr;
    void (*decDestroy)(void*) = nullptr;
    void (*decode)(void*, const unsigned char*, int, short*) = nullptr;
    void (*decFlush)(void*, short*) = nullptr;
    // v3.8's 16 kHz coder.
    void* (*enc16Create)() = nullptr;
    void (*enc16Destroy)(void*) = nullptr;
    int (*encode16)(void*, const short*, unsigned char*) = nullptr;
    int (*enc16Flush)(void*, unsigned char*) = nullptr;
    void* (*dec16Create)() = nullptr;
    void (*dec16Destroy)(void*) = nullptr;
    void (*decode16)(void*, const unsigned char*, int, short*) = nullptr;
    int (*dec16Flush)(void*, short*) = nullptr;
    // sotto 1200 (sotto_lr_*), optional.
    void* (*lrEncCreate)() = nullptr;
    void (*lrEncDestroy)(void*) = nullptr;
    int (*lrEncode)(void*, const short*, unsigned char*) = nullptr;
    void (*lrEncodePart)(void*, const short*, int) = nullptr;
    int (*lrEncFlush)(void*, unsigned char*) = nullptr;
    void* (*lrDecCreate)() = nullptr;
    void (*lrDecDestroy)(void*) = nullptr;
    void (*lrDecode)(void*, const unsigned char*, int, short*) = nullptr;
    int (*lrDecFlush)(void*, short*) = nullptr;
    bool lr = false;
    // sotto 600 and 400 (sotto_vlr_*, sotto_vlr.h), optional: the encoder
    // and the decoder apart, as a library may have only the encoder.
    void* (*vlrEncCreate)(int) = nullptr;
    void (*vlrEncDestroy)(void*) = nullptr;
    void (*vlrEncode)(void*, const short*, int) = nullptr;
    int (*vlrEncPop)(void*, unsigned char*) = nullptr;
    int (*vlrEncFlush)(void*, unsigned char*) = nullptr;
    void* (*vlrDecCreate)(int) = nullptr;
    void (*vlrDecDestroy)(void*) = nullptr;
    int (*vlrSamples)(const void*) = nullptr;
    void (*vlrDecode)(void*, const unsigned char*, int, short*) = nullptr;
    int (*vlrDecFlush)(void*, short*) = nullptr;
    bool vlrEnc = false;
    bool vlrDec = false;
};

namespace
{

#ifdef _WIN32
constexpr const char* LIBRARY_NAME = "sotto.dll";

std::string programFolder()
{
    std::vector<wchar_t> path(MAX_PATH);
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (n == 0) return std::string();
        if (n < path.size()) break;
        path.resize(path.size() * 2);
    }
    std::wstring wide(path.data());
    size_t slash = wide.find_last_of(L"\\/");
    if (slash != std::wstring::npos) wide.resize(slash + 1);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)std::max(0, bytes - 1), '\0');
    if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], bytes, nullptr, nullptr);
    return utf8;
}

void* openLibrary(const std::string& file, std::string& error)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
    std::wstring wide((size_t)std::max(0, n - 1), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &wide[0], n);
    // The folder the DLL is in is searched for its own DLLs (libcodec2).
    HMODULE lib = LoadLibraryExW(wide.c_str(), nullptr,
                                 file.find_first_of("\\/") != std::string::npos
                                     ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                     : 0);
    if (!lib) error = "Windows error " + std::to_string((unsigned long)GetLastError());
    return (void*)lib;
}

void* symbol(void* lib, const char* name)
{
    return (void*)GetProcAddress((HMODULE)lib, name);
}

void closeLibrary(void* lib)
{
    FreeLibrary((HMODULE)lib);
}
#else
constexpr const char* LIBRARY_NAME = "libsotto.so.0";

std::string programFolder()
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return std::string();
    path[n] = '\0';
    std::string s(path);
    size_t slash = s.rfind('/');
    if (slash != std::string::npos) s.resize(slash + 1);
    return s;
}

void* openLibrary(const std::string& file, std::string& error)
{
    void* lib = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
    }
    return lib;
}

void* symbol(void* lib, const char* name)
{
    return dlsym(lib, name);
}

void closeLibrary(void* lib)
{
    dlclose(lib);
}
#endif

bool fileExists(const std::string& file)
{
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

template <typename F>
bool bind(void* lib, const char* name, F& f)
{
    f = reinterpret_cast<F>(symbol(lib, name));
    return f != nullptr;
}

// sotto 600's and 400's rate argument.
int vlrRate(uint8_t codec)
{
    return codec == CODEC_SOTTO_400 ? 400 : 600;
}

// v3.7's 8 kHz decoder core gives a frame's sound 20 ms late (sotto.h);
// sotto 600 and 400 add it to their two superframes (sotto_vlr.h), while
// sotto 1200's decoder is two superframes behind in all (sotto.h).
constexpr int CORE_DELAY_8K = 160;

} // namespace

std::vector<std::string> sottoCandidates()
{
    std::vector<std::string> out;
    if (const char* env = std::getenv("RAGTIME_SOTTO"))
    {
        if (*env) out.push_back(env);
    }
    std::string here = programFolder();
    if (!here.empty())
    {
        out.push_back(here + LIBRARY_NAME);
#ifndef _WIN32
        // The AppImage keeps its libraries in usr/lib beside usr/bin.
        out.push_back(here + "../lib/" + LIBRARY_NAME);
        // Built from source: packaging/build-sotto.sh leaves it in the
        // checkout's build-sotto/out, a folder or a few above the program
        // (build/app/ragtime, build/tests/...).
        std::string up = here;
        for (int level = 0; level < 4 && up.size() > 1; level++)
        {
            up.pop_back();     // the trailing '/'
            size_t slash = up.rfind('/');
            if (slash == std::string::npos) break;
            up.resize(slash + 1);
            out.push_back(up + "build-sotto/out/" + LIBRARY_NAME);
        }
#endif
    }
    out.push_back(LIBRARY_NAME);    // the system's search path
    return out;
}

//-------------------------------------------------------------------------
// Sotto
//-------------------------------------------------------------------------

Sotto& Sotto::library()
{
    static Sotto lib("");
    return lib;
}

Sotto::Sotto(const std::string& path)
{
    std::vector<std::string> tries = path.empty() ? sottoCandidates() : std::vector<std::string>{path};
    std::string foundWhy;
    for (const std::string& file : tries)
    {
        std::string why;
        if (open(file, why)) return;
        // A library there but refused (a missing libcodec2, the wrong
        // architecture, an older sotto) says more than "not found" on
        // every other path; the next place may still have a good one.
        bool there = file.find_first_of("\\/") != std::string::npos ? fileExists(file)
                     : why.find("No such file") == std::string::npos
                           && why.find("Windows error 126") == std::string::npos;
        if (there && foundWhy.empty()) foundWhy = why;
    }
    if (!foundWhy.empty())
        info_.why = foundWhy;
    else if (!path.empty())
        info_.why = "libsotto isn't at " + path;
    else
#ifdef _WIN32
        info_.why = "sotto.dll isn't installed: put it beside ragtime.exe, or set RAGTIME_SOTTO to its path";
#else
        info_.why = "libsotto isn't installed: build it with packaging/build-sotto.sh in Ragtime's checkout, "
                    "or set RAGTIME_SOTTO to libsotto.so.0's path";
#endif
}

Sotto::~Sotto()
{
    close();
}

bool Sotto::open(const std::string& file, std::string& why)
{
    std::string error;
    void* lib = openLibrary(file, error);
    if (!lib)
    {
        why = "libsotto couldn't be loaded (" + error + ")";
        return false;
    }
    auto api = std::make_unique<SottoApi>();
    bool ok = bind(lib, "sotto_format", api->format) && bind(lib, "sotto_enc_create", api->encCreate)
              && bind(lib, "sotto_enc_destroy", api->encDestroy) && bind(lib, "sotto_encode", api->encode)
              && bind(lib, "sotto_encode_half", api->encodeHalf) && bind(lib, "sotto_enc_flush", api->encFlush)
              && bind(lib, "sotto_dec_create", api->decCreate) && bind(lib, "sotto_dec_destroy", api->decDestroy)
              && bind(lib, "sotto_decode", api->decode) && bind(lib, "sotto_dec_flush", api->decFlush)
              && bind(lib, "sotto_enc16_create", api->enc16Create)
              && bind(lib, "sotto_enc16_destroy", api->enc16Destroy) && bind(lib, "sotto_encode16", api->encode16)
              && bind(lib, "sotto_enc16_flush", api->enc16Flush) && bind(lib, "sotto_dec16_create", api->dec16Create)
              && bind(lib, "sotto_dec16_destroy", api->dec16Destroy) && bind(lib, "sotto_decode16", api->decode16)
              && bind(lib, "sotto_dec16_flush", api->dec16Flush);
    if (!ok)
    {
        closeLibrary(lib);
        why = file + " isn't a libsotto Ragtime can use (a function is missing: Ragtime needs sotto v3.8, "
                     "which packaging/build-sotto.sh builds)";
        return false;
    }
    api->lr = bind(lib, "sotto_lr_enc_create", api->lrEncCreate)
              && bind(lib, "sotto_lr_enc_destroy", api->lrEncDestroy) && bind(lib, "sotto_lr_encode", api->lrEncode)
              && bind(lib, "sotto_lr_encode_part", api->lrEncodePart)
              && bind(lib, "sotto_lr_enc_flush", api->lrEncFlush) && bind(lib, "sotto_lr_dec_create", api->lrDecCreate)
              && bind(lib, "sotto_lr_dec_destroy", api->lrDecDestroy) && bind(lib, "sotto_lr_decode", api->lrDecode)
              && bind(lib, "sotto_lr_dec_flush", api->lrDecFlush);
    api->vlrEnc = bind(lib, "sotto_vlr_enc_create", api->vlrEncCreate)
                  && bind(lib, "sotto_vlr_enc_destroy", api->vlrEncDestroy)
                  && bind(lib, "sotto_vlr_encode", api->vlrEncode) && bind(lib, "sotto_vlr_enc_pop", api->vlrEncPop)
                  && bind(lib, "sotto_vlr_enc_flush", api->vlrEncFlush);
    api->vlrDec = bind(lib, "sotto_vlr_dec_create", api->vlrDecCreate)
                  && bind(lib, "sotto_vlr_dec_destroy", api->vlrDecDestroy)
                  && bind(lib, "sotto_vlr_samples", api->vlrSamples) && bind(lib, "sotto_vlr_decode", api->vlrDecode)
                  && bind(lib, "sotto_vlr_dec_flush", api->vlrDecFlush);
    const char* format = api->format();
    std::string name = format ? format : "";
    uint8_t codec = codecOfFormat(name);
    if (codec == 0)
    {
        closeLibrary(lib);
        why = file + " speaks " + (name.empty() ? std::string("an unnamed format") : "\"" + name + "\"")
              + ", not sotto v3.8 (codec 2): packaging/build-sotto.sh builds the one Ragtime needs";
        return false;
    }
    handle_ = lib;
    info_.available = true;
    info_.format = name;
    info_.codec = codec;
    info_.path = file;
    info_.why.clear();
    info_.plays = {CODEC_SOTTO_V36, CODEC_SOTTO_V38};
    info_.sends = {CODEC_SOTTO_V38};
    if (api->lr)
    {
        info_.plays.push_back(CODEC_SOTTO_1200);
        info_.sends.push_back(CODEC_SOTTO_1200);
    }
    if (api->vlrDec)
    {
        info_.plays.push_back(CODEC_SOTTO_600);
        info_.plays.push_back(CODEC_SOTTO_400);
        if (api->vlrEnc)
        {
            info_.sends.push_back(CODEC_SOTTO_600);
            info_.sends.push_back(CODEC_SOTTO_400);
        }
    }
    api_ = std::move(api);
    return true;
}

bool Sotto::sends(uint8_t codec) const
{
    return std::find(info_.sends.begin(), info_.sends.end(), codec) != info_.sends.end();
}

bool Sotto::plays(uint8_t codec) const
{
    return std::find(info_.plays.begin(), info_.plays.end(), codec) != info_.plays.end();
}

void Sotto::close()
{
    if (handle_) closeLibrary(handle_);
    handle_ = nullptr;
    api_.reset();
}

std::unique_ptr<SottoEncoder> Sotto::encoder(uint8_t codec) const
{
    if (!api_ || !sends(codec)) return nullptr;
    void* state = nullptr;
    if (codec == CODEC_SOTTO_V38) state = api_->enc16Create();
    else if (codec == CODEC_SOTTO_1200) state = api_->lrEncCreate();
    else state = api_->vlrEncCreate(vlrRate(codec));
    if (!state) return nullptr;
    return std::unique_ptr<SottoEncoder>(new SottoEncoder(api_.get(), codec, state));
}

std::unique_ptr<SottoDecoder> Sotto::decoder(uint8_t codec) const
{
    if (!api_ || !plays(codec)) return nullptr;
    std::unique_ptr<SottoDecoder> d(new SottoDecoder(api_.get(), codec));
    if (!d->state_) return nullptr;
    return d;
}

//-------------------------------------------------------------------------
// SottoEncoder
//-------------------------------------------------------------------------

// 16 kHz speech halved to 8 kHz for the slower codecs.
struct SottoEncoder::Halver
{
    Resampler down{SOTTO_RATE, 8000};
    std::vector<float> in, out;

    // The filter's delay (about 6 ms), taken off the front so the frames
    // keep time with the speech.
    size_t skip = down.delayIn() / 2;

    void push(const int16_t* samples, size_t count, std::vector<int16_t>& to)
    {
        in.resize(count);
        for (size_t i = 0; i < count; i++) in[i] = samples[i] / 32768.0f;
        out.clear();
        down.process(in.data(), in.size(), out);
        for (float v : out)
        {
            if (skip > 0)
            {
                skip--;
                continue;
            }
            to.push_back((int16_t)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f));
        }
    }
};

SottoEncoder::SottoEncoder(const SottoApi* api, uint8_t codec, void* state) : api_(api), codec_(codec), state_(state)
{
    if (codec_ == CODEC_SOTTO_V38) return;
    halver_ = std::make_unique<Halver>();
    // sotto 1200's encoder puts a superframe 20 ms after its speech (its
    // decoder is two superframes behind, not the core's 20 ms more), so
    // its first 20 ms go too: the clip keeps time, and its end isn't cut.
    if (codec_ == CODEC_SOTTO_1200) halver_->skip += CORE_DELAY_8K;
}

SottoEncoder::~SottoEncoder()
{
    if (!state_) return;
    if (codec_ == CODEC_SOTTO_V38) api_->enc16Destroy(state_);
    else if (codec_ == CODEC_SOTTO_1200) api_->lrEncDestroy(state_);
    else api_->vlrEncDestroy(state_);
}

size_t SottoEncoder::blockSamples() const
{
    // sotto_encode16 and sotto_lr_encode take 320 (20 ms at 16 kHz, 40 ms
    // at 8 kHz); sotto_vlr_encode takes 10 ms blocks of 80.
    return codec_ == CODEC_SOTTO_V38 || codec_ == CODEC_SOTTO_1200 ? 320 : 80;
}

void SottoEncoder::block(const int16_t* samples, std::vector<SottoFrame>& out)
{
    SottoFrame f;
    const short* x = reinterpret_cast<const short*>(samples);
    if (codec_ == CODEC_SOTTO_V38)
    {
        if (api_->encode16(state_, x, f.data()) > 0) out.push_back(f);
    }
    else if (codec_ == CODEC_SOTTO_1200)
    {
        if (api_->lrEncode(state_, x, f.data()) > 0) out.push_back(f);
    }
    else
    {
        api_->vlrEncode(state_, x, 1);
        while (api_->vlrEncPop(state_, f.data()) > 0) out.push_back(f);
    }
}

void SottoEncoder::coded(const int16_t* samples, size_t count, std::vector<SottoFrame>& out)
{
    const size_t n = blockSamples();
    size_t at = 0;
    // Top up a part block left from before.
    if (!part_.empty())
    {
        size_t take = std::min(count, n - part_.size());
        part_.insert(part_.end(), samples, samples + take);
        at = take;
        if (part_.size() < n) return;
        block(part_.data(), out);
        part_.clear();
    }
    while (count - at >= n)
    {
        block(samples + at, out);
        at += n;
    }
    part_.assign(samples + at, samples + count);
}

void SottoEncoder::push(const int16_t* samples, size_t count, std::vector<SottoFrame>& out)
{
    if (flushed_) return;
    if (!halver_)
    {
        coded(samples, count, out);
        return;
    }
    std::vector<int16_t> half;
    halver_->push(samples, count, half);
    coded(half.data(), half.size(), out);
}

void SottoEncoder::flush(std::vector<SottoFrame>& out)
{
    if (flushed_) return;
    if (halver_)
    {
        // The halver's own delay of speech, out with silence after it.
        std::vector<int16_t> zeros(halver_->down.delayIn() + 2, 0), half;
        halver_->push(zeros.data(), zeros.size(), half);
        coded(half.data(), half.size(), out);
    }
    SottoFrame f;
    if (codec_ == CODEC_SOTTO_1200)
    {
        // sotto 1200 takes the last 10 ms blocks as they are.
        if (part_.size() >= 80) api_->lrEncodePart(state_, reinterpret_cast<const short*>(part_.data()), (int)(part_.size() / 80));
        part_.clear();
        while (api_->lrEncFlush(state_, f.data()) > 0) out.push_back(f);
    }
    else
    {
        if (!part_.empty())
        {
            part_.resize(blockSamples(), 0);
            block(part_.data(), out);
            part_.clear();
        }
        if (codec_ == CODEC_SOTTO_V38)
            while (api_->enc16Flush(state_, f.data()) > 0) out.push_back(f);
        else
            while (api_->vlrEncFlush(state_, f.data()) > 0) out.push_back(f);
    }
    flushed_ = true;
}

//-------------------------------------------------------------------------
// SottoDecoder
//-------------------------------------------------------------------------

// The 8 kHz codecs' sound doubled to 16 kHz.
struct SottoDecoder::Doubler
{
    Resampler up{8000, SOTTO_RATE};
    std::vector<float> in, out;

    // The filter's delay (about 6 ms), taken off the front: give() makes
    // it up with silence at the start, so the sound keeps time.
    size_t skip = up.delayIn() * 2;

    void push(const int16_t* samples, size_t count, std::vector<int16_t>& to)
    {
        in.resize(count);
        for (size_t i = 0; i < count; i++) in[i] = samples[i] / 32768.0f;
        out.clear();
        up.process(in.data(), in.size(), out);
        for (float v : out)
        {
            if (skip > 0)
            {
                skip--;
                continue;
            }
            to.push_back((int16_t)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f));
        }
    }
};

SottoDecoder::SottoDecoder(const SottoApi* api, uint8_t codec) : api_(api), codec_(codec)
{
    const Codec* c = codecOf(codec);
    frameSamples_ = (size_t)(SOTTO_RATE / 1000 * (c ? c->frameMs : FRAME_MS));
    create();
}

SottoDecoder::~SottoDecoder()
{
    destroy();
}

void SottoDecoder::create()
{
    held_ = 0;
    drained_ = false;
    fifo_.clear();
    doubler_.reset();
    // How far behind sotto's own decoder is, at 16 kHz.
    size_t behind = 0;
    switch (codec_)
    {
        case CODEC_SOTTO_V36:
            state_ = api_->decCreate();
            behind = 2 * CORE_DELAY_8K;
            break;
        case CODEC_SOTTO_V38:
            state_ = api_->dec16Create();
            behind = (size_t)SOTTO_DEC16_DELAY * SOTTO_SAMPLES;
            break;
        case CODEC_SOTTO_1200:
            state_ = api_->lrDecCreate();
            behind = 2 * frameSamples_;
            break;
        default:
            state_ = api_->vlrDecCreate(vlrRate(codec_));
            behind = 2 * frameSamples_ + 2 * CORE_DELAY_8K;
            break;
    }
    if (codec_ != CODEC_SOTTO_V38) doubler_ = std::make_unique<Doubler>();
    // Whole frames behind: the part frame is silence ahead of the sound.
    delay_ = (int)((behind + frameSamples_ - 1) / frameSamples_);
    fifo_.assign((size_t)delay_ * frameSamples_ - behind, 0);
}

void SottoDecoder::destroy()
{
    if (state_)
    {
        switch (codec_)
        {
            case CODEC_SOTTO_V36:
                api_->decDestroy(state_);
                break;
            case CODEC_SOTTO_V38:
                api_->dec16Destroy(state_);
                break;
            case CODEC_SOTTO_1200:
                api_->lrDecDestroy(state_);
                break;
            default:
                api_->vlrDecDestroy(state_);
                break;
        }
    }
    state_ = nullptr;
    doubler_.reset();
}

void SottoDecoder::take(const int16_t* raw, size_t count)
{
    if (doubler_) doubler_->push(raw, count, fifo_);
    else fifo_.insert(fifo_.end(), raw, raw + count);
}

void SottoDecoder::give(int16_t* samples)
{
    const size_t have = std::min(fifo_.size(), frameSamples_);
    const size_t lead = frameSamples_ - have;
    std::fill(samples, samples + lead, (int16_t)0);
    std::copy(fifo_.begin(), fifo_.begin() + (std::ptrdiff_t)have, samples + lead);
    fifo_.erase(fifo_.begin(), fifo_.begin() + (std::ptrdiff_t)have);
}

void SottoDecoder::decode(const SottoFrame& frame, bool good, int16_t* samples)
{
    const int ok = good ? 1 : 0;
    std::vector<int16_t> raw;
    switch (codec_)
    {
        case CODEC_SOTTO_V36:
            raw.resize(SOTTO_SAMPLES / 2);
            api_->decode(state_, frame.data(), ok, reinterpret_cast<short*>(raw.data()));
            break;
        case CODEC_SOTTO_V38:
            raw.resize(SOTTO_SAMPLES);
            api_->decode16(state_, frame.data(), ok, reinterpret_cast<short*>(raw.data()));
            break;
        case CODEC_SOTTO_1200:
            raw.resize(frameSamples_ / 2);
            api_->lrDecode(state_, frame.data(), ok, reinterpret_cast<short*>(raw.data()));
            break;
        default:
            raw.resize((size_t)std::max(0, api_->vlrSamples(state_)));
            api_->vlrDecode(state_, frame.data(), ok, reinterpret_cast<short*>(raw.data()));
            break;
    }
    take(raw.data(), raw.size());
    give(samples);
    held_ = std::min(held_ + 1, delay_);
}

bool SottoDecoder::flush(int16_t* samples)
{
    if (held_ <= 0) return false;
    if (!drained_)
    {
        std::vector<int16_t> raw;
        switch (codec_)
        {
            case CODEC_SOTTO_V36:
                // sotto_dec_flush gives its last 20 ms once.
                raw.resize(SOTTO_SAMPLES / 2);
                api_->decFlush(state_, reinterpret_cast<short*>(raw.data()));
                drained_ = true;
                break;
            case CODEC_SOTTO_V38:
                raw.resize(SOTTO_SAMPLES);
                if (api_->dec16Flush(state_, reinterpret_cast<short*>(raw.data())) <= 0) raw.clear();
                break;
            case CODEC_SOTTO_1200:
                raw.resize(frameSamples_ / 2);
                if (api_->lrDecFlush(state_, reinterpret_cast<short*>(raw.data())) <= 0) raw.clear();
                break;
            default:
                raw.resize((size_t)std::max(0, api_->vlrSamples(state_)));
                if (api_->vlrDecFlush(state_, reinterpret_cast<short*>(raw.data())) <= 0) raw.clear();
                break;
        }
        if (raw.empty()) drained_ = true;
        else take(raw.data(), raw.size());
    }
    give(samples);
    held_--;
    return true;
}

std::vector<int16_t> SottoDecoder::decodeClip(const std::vector<SottoFrame>& frames, const std::vector<uint8_t>& good)
{
    // A fresh state, so an earlier clip's tail doesn't run into this one.
    destroy();
    create();
    std::vector<int16_t> out;
    if (frames.empty() || !state_) return out;
    const size_t total = frames.size() * frameSamples_;
    out.reserve(total);
    std::vector<int16_t> sound(frameSamples_);
    // Each call gives the sound of the frame delay() calls before: the
    // first calls' sound is the delay, thrown away.
    size_t calls = 0;
    const size_t skip = (size_t)delay();
    auto keep = [&]() {
        if (calls++ >= skip) out.insert(out.end(), sound.begin(), sound.end());
    };
    for (size_t i = 0; i < frames.size(); i++)
    {
        bool ok = good.empty() || (i < good.size() && good[i] != 0);
        decode(frames[i], ok, sound.data());
        keep();
    }
    while (out.size() < total && flush(sound.data())) keep();
    out.resize(total, 0);
    return out;
}

std::vector<float> toFloat(const std::vector<int16_t>& samples)
{
    std::vector<float> out(samples.size());
    for (size_t i = 0; i < samples.size(); i++) out[i] = samples[i] / 32768.0f;
    return out;
}

} // namespace voice
} // namespace ragtime
