// The WASAPI pieces the Ear and voice share: the interfaces' ids, a COM
// pointer, text in UTF-8, the devices' names, and reading a card's samples
// in its mix format. Internal to ragtime_audio, Windows only.
#ifndef RAGTIME_AUDIO_WASAPI_COMMON_H
#define RAGTIME_AUDIO_WASAPI_COMMON_H

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ragtime
{
namespace audio
{
namespace wasapi
{

// The interfaces' ids, written out so neither __uuidof nor libuuid is needed.
inline const GUID MM_DEVICE_ENUMERATOR = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
inline const GUID I_MM_DEVICE_ENUMERATOR = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
inline const GUID I_AUDIO_CLIENT = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
inline const GUID I_AUDIO_CAPTURE_CLIENT = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
inline const GUID I_AUDIO_SESSION_MANAGER2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
inline const GUID I_AUDIO_SESSION_CONTROL2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
// PKEY_Device_FriendlyName: "USB Audio CODEC" and the like.
inline const PROPERTYKEY FRIENDLY_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

inline const GUID I_AUDIO_RENDER_CLIENT = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
// PKEY_Device_ContainerId: the physical device, shared by its inputs and
// outputs ({00000000-0000-0000-FFFF-FFFFFFFFFFFF} for everything built in).
inline const PROPERTYKEY CONTAINER_ID = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

// A COM pointer, released when it goes.
template <typename T>
class Com
{
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put()
    {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* release()
    {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void reset()
    {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

inline std::string utf8(const wchar_t* text)
{
    if (!text || !*text) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out((std::size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

inline std::wstring wide(const std::string& text)
{
    if (text.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out((std::size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
    return out;
}

inline int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A process's program, as PipeWire would name it: "data2g-host" for
// C:\...\data2g-host.exe.
inline std::string programOf(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = utf8(path);
    CloseHandle(process);
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.erase(name.size() - 4);
    return name;
}

// The device's own id, "{0.0.1.00000000}.{...}": the source's name.
inline std::string deviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return "";
    std::string out = utf8(id);
    CoTaskMemFree(id);
    return out;
}

inline std::string friendlyName(IMMDevice* device)
{
    Com<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put()))) return "";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(props->GetValue(FRIENDLY_NAME, &value)) && value.vt == VT_LPWSTR) out = utf8(value.pwszVal);
    PropVariantClear(&value);
    return out;
}

// How the card's samples are laid out, from its mix format.
struct Layout
{
    bool ok = false;
    bool isFloat = false;
    int bits = 0;           // the container's: 16, 24 or 32
    int channels = 0;
    int stride = 0;         // bytes a frame
    int rate = 0;
};

inline Layout layoutOf(const WAVEFORMATEX* w)
{
    Layout l;
    if (!w || w->nChannels == 0) return l;
    l.channels = w->nChannels;
    l.rate = (int)w->nSamplesPerSec;
    l.stride = w->nBlockAlign;
    l.bits = w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        l.isFloat = true;
    else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22)
    {
        // KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT differ only in Data1.
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        if (x->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT) l.isFloat = true;
        else if (x->SubFormat.Data1 != WAVE_FORMAT_PCM) return l;
    }
    else if (w->wFormatTag != WAVE_FORMAT_PCM)
        return l;
    l.ok = (l.isFloat ? l.bits == 32 : (l.bits == 16 || l.bits == 24 || l.bits == 32)) &&
           l.stride >= l.channels * l.bits / 8 && l.rate > 0;
    return l;
}

inline float sampleAt(const BYTE* p, const Layout& l)
{
    if (l.isFloat)
    {
        float f;
        std::memcpy(&f, p, sizeof f);
        return f;
    }
    if (l.bits == 16)
    {
        int16_t v;
        std::memcpy(&v, p, sizeof v);
        return (float)v / 32768.0f;
    }
    if (l.bits == 24)
    {
        const int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
        return (float)((double)v / 2147483648.0);
    }
    int32_t v;
    std::memcpy(&v, p, sizeof v);
    return (float)((double)v / 2147483648.0);
}

// The physical device an endpoint belongs to, as text; empty for the
// built-in devices, which all share one container.
inline std::string containerId(IMMDevice* device)
{
    Com<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.put()))) return "";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(props->GetValue(CONTAINER_ID, &value)) && value.vt == VT_CLSID && value.puuid)
    {
        const GUID& g = *value.puuid;
        const bool builtIn = g.Data1 == 0 && g.Data2 == 0 && g.Data3 == 0 && g.Data4[0] == 0 && g.Data4[1] == 0 &&
                             g.Data4[2] == 0xff && g.Data4[3] == 0xff && g.Data4[4] == 0xff && g.Data4[5] == 0xff &&
                             g.Data4[6] == 0xff && g.Data4[7] == 0xff;
        if (!builtIn)
        {
            char text[40];
            std::snprintf(text, sizeof text, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                          (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                          g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
            out = text;
        }
    }
    PropVariantClear(&value);
    return out;
}

} // namespace wasapi
} // namespace audio
} // namespace ragtime

#endif
