// libsotto loaded at run time: see Sotto.h.
#include "ragtime/voice/Sotto.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
    std::string firstWhy;
    for (const std::string& file : tries)
    {
        std::string why;
        if (open(file, why)) return;
        // The most telling reason: a library found but refused says more
        // than "not found" on every other path.
        bool found = why.find("No such file") == std::string::npos && why.find("Windows error 126") == std::string::npos;
        if (firstWhy.empty() || found) firstWhy = why;
        if (found && !why.empty()) break;
    }
    info_.why = firstWhy.empty() ? "libsotto isn't installed" : firstWhy;
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
              && bind(lib, "sotto_decode", api->decode) && bind(lib, "sotto_dec_flush", api->decFlush);
    if (!ok)
    {
        closeLibrary(lib);
        why = file + " isn't a libsotto Ragtime can use (a function is missing)";
        return false;
    }
    const char* format = api->format();
    std::string name = format ? format : "";
    uint8_t codec = codecOfFormat(name);
    if (codec == 0)
    {
        closeLibrary(lib);
        why = file + " speaks " + (name.empty() ? std::string("an unnamed format") : "\"" + name + "\"")
              + ", not sotto v3.6 (codec 1)";
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
    void* state = api_->encCreate();
    if (!state) return nullptr;
    return std::unique_ptr<SottoEncoder>(new SottoEncoder(api_.get(), state));
}

std::unique_ptr<SottoDecoder> Sotto::decoder() const
{
    if (!api_) return nullptr;
    void* state = api_->decCreate();
    if (!state) return nullptr;
    return std::unique_ptr<SottoDecoder>(new SottoDecoder(api_.get(), state));
}

//-------------------------------------------------------------------------
// SottoEncoder
//-------------------------------------------------------------------------

SottoEncoder::~SottoEncoder()
{
    if (state_) api_->encDestroy(state_);
}

bool SottoEncoder::encode(const int16_t* samples, SottoFrame& out)
{
    if (flushed_) return false;
    return api_->encode(state_, reinterpret_cast<const short*>(samples), out.data()) > 0;
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
    while (api_->encFlush(state_, f.data()) > 0) out.push_back(f);
    flushed_ = true;
}

//-------------------------------------------------------------------------
// SottoDecoder
//-------------------------------------------------------------------------

SottoDecoder::~SottoDecoder()
{
    if (state_) api_->decDestroy(state_);
}

void SottoDecoder::decode(const SottoFrame& frame, bool good, int16_t* samples)
{
    api_->decode(state_, frame.data(), good ? 1 : 0, reinterpret_cast<short*>(samples));
}

void SottoDecoder::flush(int16_t* samples)
{
    api_->decFlush(state_, reinterpret_cast<short*>(samples));
}

std::vector<int16_t> SottoDecoder::decodeClip(const std::vector<SottoFrame>& frames, const std::vector<uint8_t>& good)
{
    // A fresh state, so an earlier clip's tail doesn't run into this one.
    api_->decDestroy(state_);
    state_ = api_->decCreate();
    std::vector<int16_t> out;
    if (frames.empty() || !state_) return out;
    out.resize(frames.size() * SOTTO_SAMPLES);
    std::vector<int16_t> first(SOTTO_SAMPLES);
    // Each call gives the frame before's 20 ms: the first call's output is
    // the delay, thrown away.
    for (size_t i = 0; i < frames.size(); i++)
    {
        bool ok = good.empty() || (i < good.size() && good[i] != 0);
        int16_t* to = i == 0 ? first.data() : &out[(i - 1) * SOTTO_SAMPLES];
        decode(frames[i], ok, to);
    }
    flush(&out[(frames.size() - 1) * SOTTO_SAMPLES]);
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
