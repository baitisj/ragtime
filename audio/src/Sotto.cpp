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
    api_ = std::move(api);
    info_.available = true;
    info_.format = name;
    info_.codec = codec;
    info_.path = file;
    info_.why.clear();
    return true;
}

void Sotto::close()
{
    if (handle_) closeLibrary(handle_);
    handle_ = nullptr;
    api_.reset();
}

std::unique_ptr<SottoEncoder> Sotto::encoder() const
{
    if (!api_) return nullptr;
    void* state = api_->enc16Create();
    if (!state) return nullptr;
    return std::unique_ptr<SottoEncoder>(new SottoEncoder(api_.get(), state));
}

std::unique_ptr<SottoDecoder> Sotto::decoder(uint8_t codec) const
{
    if (!api_ || !knownCodec(codec)) return nullptr;
    std::unique_ptr<SottoDecoder> d(new SottoDecoder(api_.get(), codec));
    if (!d->state_) return nullptr;
    return d;
}

//-------------------------------------------------------------------------
// SottoEncoder
//-------------------------------------------------------------------------

SottoEncoder::~SottoEncoder()
{
    if (state_) api_->enc16Destroy(state_);
}

bool SottoEncoder::encode(const int16_t* samples, SottoFrame& out)
{
    if (flushed_) return false;
    return api_->encode16(state_, reinterpret_cast<const short*>(samples), out.data()) > 0;
}

void SottoEncoder::push(const int16_t* samples, size_t count, std::vector<SottoFrame>& out)
{
    if (flushed_) return;
    size_t at = 0;
    // Top up a part frame left from before.
    if (!part_.empty())
    {
        size_t take = std::min(count, (size_t)SOTTO_SAMPLES - part_.size());
        part_.insert(part_.end(), samples, samples + take);
        at = take;
        if (part_.size() < (size_t)SOTTO_SAMPLES) return;
        SottoFrame f;
        if (encode(part_.data(), f)) out.push_back(f);
        part_.clear();
    }
    while (count - at >= (size_t)SOTTO_SAMPLES)
    {
        SottoFrame f;
        if (encode(samples + at, f)) out.push_back(f);
        at += SOTTO_SAMPLES;
    }
    part_.assign(samples + at, samples + count);
}

void SottoEncoder::flush(std::vector<SottoFrame>& out)
{
    if (flushed_) return;
    if (!part_.empty())
    {
        part_.resize(SOTTO_SAMPLES, 0);
        SottoFrame f;
        if (encode(part_.data(), f)) out.push_back(f);
        part_.clear();
    }
    SottoFrame f;
    while (api_->enc16Flush(state_, f.data()) > 0) out.push_back(f);
    flushed_ = true;
}

//-------------------------------------------------------------------------
// SottoDecoder
//-------------------------------------------------------------------------

// Codec 1's 8 kHz sound doubled to 16 kHz, a frame a call.
struct SottoDecoder::Doubler
{
    Resampler up{8000, SOTTO_RATE};
    std::vector<float> in, out;
    std::vector<float> fifo;

    void push(const int16_t* half, int16_t* samples)
    {
        in.assign(half, half + SOTTO_SAMPLES / 2);
        for (float& v : in) v /= 32768.0f;
        out.clear();
        up.process(in.data(), in.size(), out);
        fifo.insert(fifo.end(), out.begin(), out.end());
        // The resampler gives two samples a sample, its few samples of
        // delay riding along (well under a millisecond).
        const size_t n = std::min(fifo.size(), (size_t)SOTTO_SAMPLES);
        size_t at = 0;
        for (; at < (size_t)SOTTO_SAMPLES - n; at++) samples[at] = 0;
        for (size_t i = 0; i < n; i++, at++)
        {
            const float v = std::max(-1.0f, std::min(1.0f, fifo[i])) * 32767.0f;
            samples[at] = (int16_t)std::lround(v);
        }
        fifo.erase(fifo.begin(), fifo.begin() + (std::ptrdiff_t)n);
    }
};

SottoDecoder::SottoDecoder(const SottoApi* api, uint8_t codec) : api_(api), codec_(codec)
{
    create();
}

SottoDecoder::~SottoDecoder()
{
    destroy();
}

void SottoDecoder::create()
{
    held_ = 0;
    if (codec_ == CODEC_V36)
    {
        state_ = api_->decCreate();
        doubler_ = std::make_unique<Doubler>();
    }
    else
    {
        state_ = api_->dec16Create();
    }
}

void SottoDecoder::destroy()
{
    if (state_) (codec_ == CODEC_V36 ? api_->decDestroy : api_->dec16Destroy)(state_);
    state_ = nullptr;
    doubler_.reset();
}

void SottoDecoder::decode(const SottoFrame& frame, bool good, int16_t* samples)
{
    if (codec_ == CODEC_V36)
    {
        int16_t half[SOTTO_SAMPLES / 2];
        api_->decode(state_, frame.data(), good ? 1 : 0, reinterpret_cast<short*>(half));
        doubler_->push(half, samples);
    }
    else
    {
        api_->decode16(state_, frame.data(), good ? 1 : 0, reinterpret_cast<short*>(samples));
    }
    held_ = std::min(held_ + 1, delay());
}

bool SottoDecoder::flush(int16_t* samples)
{
    if (held_ <= 0) return false;
    if (codec_ == CODEC_V36)
    {
        int16_t half[SOTTO_SAMPLES / 2];
        api_->decFlush(state_, reinterpret_cast<short*>(half));
        doubler_->push(half, samples);
    }
    else if (api_->dec16Flush(state_, reinterpret_cast<short*>(samples)) <= 0)
    {
        held_ = 0;
        return false;
    }
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
    out.reserve(frames.size() * SOTTO_SAMPLES);
    std::vector<int16_t> sound(SOTTO_SAMPLES);
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
    while (out.size() < frames.size() * SOTTO_SAMPLES && flush(sound.data())) keep();
    out.resize(frames.size() * SOTTO_SAMPLES, 0);
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
