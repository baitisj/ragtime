#include "ragtime/image/Codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>

#include <openjpeg.h>
#include <webp/decode.h>
#if RAGTIME_HEIF
#include <libheif/heif.h>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#endif

namespace ragtime
{
namespace image
{

namespace
{

// Pictures read from disk may be this big at most (a 48 megapixel phone
// camera is well under it), so a hostile file can't ask for gigabytes.
constexpr uint64_t MAX_LOAD_PIXELS = 64ull * 1000 * 1000;

//-------------------------------------------------------------------------
// OpenJPEG, through memory
//-------------------------------------------------------------------------

// What an OpenJPEG stream reads from, or writes to.
struct Memory
{
    const uint8_t* data = nullptr;   // reading
    size_t size = 0;
    std::vector<uint8_t>* out = nullptr;  // writing
    size_t at = 0;
};

OPJ_SIZE_T readMemory(void* buffer, OPJ_SIZE_T bytes, void* user)
{
    Memory& m = *static_cast<Memory*>(user);
    if (m.at >= m.size) return (OPJ_SIZE_T)-1;
    size_t n = std::min<size_t>(bytes, m.size - m.at);
    std::memcpy(buffer, m.data + m.at, n);
    m.at += n;
    return n;
}

OPJ_SIZE_T writeMemory(void* buffer, OPJ_SIZE_T bytes, void* user)
{
    Memory& m = *static_cast<Memory*>(user);
    if (m.out->size() < m.at + bytes) m.out->resize(m.at + bytes);
    std::memcpy(m.out->data() + m.at, buffer, bytes);
    m.at += bytes;
    return bytes;
}

OPJ_OFF_T skipMemory(OPJ_OFF_T bytes, void* user)
{
    Memory& m = *static_cast<Memory*>(user);
    size_t end = m.out != nullptr ? (size_t)-1 : m.size;
    if (bytes < 0)
    {
        size_t back = std::min<size_t>(m.at, (size_t)(-bytes));
        m.at -= back;
        return -(OPJ_OFF_T)back;
    }
    size_t n = std::min<size_t>((size_t)bytes, end - m.at);
    m.at += n;
    if (m.out != nullptr && m.out->size() < m.at) m.out->resize(m.at);
    return (OPJ_OFF_T)n;
}

OPJ_BOOL seekMemory(OPJ_OFF_T to, void* user)
{
    Memory& m = *static_cast<Memory*>(user);
    if (to < 0 || (m.out == nullptr && (size_t)to > m.size)) return OPJ_FALSE;
    m.at = (size_t)to;
    if (m.out != nullptr && m.out->size() < m.at) m.out->resize(m.at);
    return OPJ_TRUE;
}

opj_stream_t* readStream(Memory& m)
{
    opj_stream_t* s = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE);
    if (s == nullptr) return nullptr;
    opj_stream_set_user_data(s, &m, nullptr);
    opj_stream_set_user_data_length(s, m.size);
    opj_stream_set_read_function(s, readMemory);
    opj_stream_set_skip_function(s, skipMemory);
    opj_stream_set_seek_function(s, seekMemory);
    return s;
}

opj_stream_t* writeStream(Memory& m)
{
    opj_stream_t* s = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_FALSE);
    if (s == nullptr) return nullptr;
    opj_stream_set_user_data(s, &m, nullptr);
    opj_stream_set_write_function(s, writeMemory);
    opj_stream_set_skip_function(s, skipMemory);
    opj_stream_set_seek_function(s, seekMemory);
    return s;
}

// OpenJPEG says what went wrong through these; the last error is kept.
void quiet(const char*, void*) {}
void keepError(const char* message, void* user)
{
    std::string& error = *static_cast<std::string*>(user);
    error = message;
    while (!error.empty() && (error.back() == '\n' || error.back() == ' ')) error.pop_back();
}

void handlers(opj_codec_t* codec, std::string* error)
{
    opj_set_info_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_error_handler(codec, keepError, error);
}

struct CodecGone
{
    void operator()(opj_codec_t* c) const { opj_destroy_codec(c); }
};
struct StreamGone
{
    void operator()(opj_stream_t* s) const { opj_stream_destroy(s); }
};
struct ImageGone
{
    void operator()(opj_image_t* i) const { opj_image_destroy(i); }
};
using CodecPtr = std::unique_ptr<opj_codec_t, CodecGone>;
using StreamPtr = std::unique_ptr<opj_stream_t, StreamGone>;
using ImagePtr = std::unique_ptr<opj_image_t, ImageGone>;

// Drops the main header's comment segments (COM, 0xFF64): OpenJPEG writes
// "Created by OpenJPEG version ..." into every codestream, nearly 40 bytes
// that would go on the air with each picture.
void dropComments(std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0x4F) return;
    size_t at = 2;
    while (at + 4 <= bytes.size() && bytes[at] == 0xFF)
    {
        uint8_t marker = bytes[at + 1];
        if (marker == 0x90) return;     // SOT: the main header has ended
        size_t length = (size_t)bytes[at + 2] << 8 | bytes[at + 3];
        if (length < 2 || at + 2 + length > bytes.size()) return;
        if (marker == 0x64)
            bytes.erase(bytes.begin() + (long)at, bytes.begin() + (long)(at + 2 + length));
        else
            at += 2 + length;
    }
}

// One go at coding image so that its last layer is about target bytes.
bool encodeOnce(const picture::Image& image, double target, int layers, int resolutions,
                std::vector<uint8_t>& out, std::string& error)
{
    const int c = image.channels == 1 ? 1 : 3;
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.tcp_numlayers = layers;
    p.cp_disto_alloc = 1;
    p.irreversible = 1;
    p.prog_order = OPJ_LRCP;
    p.numresolution = resolutions;
    p.tcp_mct = c == 3 ? 1 : 0;
    double raw = (double)image.width * image.height * c;
    for (int i = 0; i < layers; i++)
    {
        double bytes = target / std::pow(2.0, layers - 1 - i);
        p.tcp_rates[i] = (float)std::max(1.0, raw / bytes);
    }

    opj_image_cmptparm_t parts[3];
    std::memset(parts, 0, sizeof parts);
    for (int k = 0; k < c; k++)
    {
        parts[k].dx = parts[k].dy = 1;
        parts[k].w = (OPJ_UINT32)image.width;
        parts[k].h = (OPJ_UINT32)image.height;
        parts[k].prec = 8;
        parts[k].sgnd = 0;
    }
    ImagePtr j2k(opj_image_create((OPJ_UINT32)c, parts, c == 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY));
    if (!j2k)
    {
        error = "out of memory";
        return false;
    }
    j2k->x0 = j2k->y0 = 0;
    j2k->x1 = (OPJ_UINT32)image.width;
    j2k->y1 = (OPJ_UINT32)image.height;
    const size_t pixels = (size_t)image.width * (size_t)image.height;
    for (int k = 0; k < c; k++)
    {
        OPJ_INT32* d = j2k->comps[k].data;
        if (image.channels == 1)
            for (size_t i = 0; i < pixels; i++) d[i] = image.pixels[i];
        else
            for (size_t i = 0; i < pixels; i++) d[i] = image.pixels[i * (size_t)image.channels + (size_t)k];
    }

    CodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    if (!codec) return false;
    handlers(codec.get(), &error);
    if (!opj_setup_encoder(codec.get(), &p, j2k.get())) return false;
    out.clear();
    Memory m;
    m.out = &out;
    StreamPtr stream(writeStream(m));
    if (!stream) return false;
    if (!opj_start_compress(codec.get(), j2k.get(), stream.get()) || !opj_encode(codec.get(), stream.get())
        || !opj_end_compress(codec.get(), stream.get()))
    {
        if (error.empty()) error = "OpenJPEG couldn't code it";
        return false;
    }
    dropComments(out);
    return true;
}

// The wavelet's levels: six (a 640 pixel picture's smallest is 20 pixels
// across, the thumbnail the first bytes draw), fewer for a small picture.
int resolutionsFor(int width, int height)
{
    int r = 6;
    while (r > 1 && (1 << (r - 1)) > std::min(width, height)) r--;
    return r;
}

// Quality layers: the first about 400 bytes, each one after it twice the
// last, eight at most.
int layersFor(uint64_t bytes)
{
    int layers = 1;
    while (layers < 8 && (double)bytes / std::pow(2.0, layers) >= 400.0) layers++;
    return layers;
}

// A decoded component's sample, at an image pixel, as 8 bits.
struct Component
{
    const opj_image_comp_t* comp;
    int shift;      // a signed component is moved up into 0..max
    double scale;   // to 0..255

    explicit Component(const opj_image_comp_t& c)
        : comp(&c)
        , shift(c.sgnd ? 1 << (std::max<OPJ_UINT32>(1, c.prec) - 1) : 0)
        , scale(255.0 / (double)((1u << std::min<OPJ_UINT32>(16, std::max<OPJ_UINT32>(1, c.prec))) - 1))
    {
    }

    uint8_t at(int x, int y) const
    {
        int cx = std::min((int)comp->w - 1, x / (int)std::max<OPJ_UINT32>(1, comp->dx));
        int cy = std::min((int)comp->h - 1, y / (int)std::max<OPJ_UINT32>(1, comp->dy));
        double v = (comp->data[(size_t)cy * comp->w + (size_t)cx] + shift) * scale;
        return (uint8_t)std::lround(std::min(255.0, std::max(0.0, v)));
    }
};

bool decodeJ2k(const uint8_t* bytes, size_t length, bool jp2, picture::Image& out, std::string& error, int maxSide)
{
    CodecPtr codec(opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K));
    if (!codec) return false;
    handlers(codec.get(), &error);
    opj_dparameters_t p;
    opj_set_default_decoder_parameters(&p);
    if (!opj_setup_decoder(codec.get(), &p)) return false;
    // A first part of a codestream is a softer picture, not an error.
    opj_decoder_set_strict_mode(codec.get(), OPJ_FALSE);
    Memory m;
    m.data = bytes;
    m.size = length;
    StreamPtr stream(readStream(m));
    if (!stream) return false;
    opj_image_t* header = nullptr;
    if (!opj_read_header(stream.get(), codec.get(), &header))
    {
        if (header != nullptr) opj_image_destroy(header);
        if (error.empty()) error = "no picture in it yet";
        return false;
    }
    ImagePtr j2k(header);
    int width = (int)(j2k->x1 - j2k->x0), height = (int)(j2k->y1 - j2k->y0);
    if (width <= 0 || height <= 0 || j2k->numcomps == 0 || j2k->numcomps > 4)
    {
        error = "not a picture Ragtime shows";
        return false;
    }
    if (width > maxSide || height > maxSide)
    {
        error = std::to_string(width) + " by " + std::to_string(height) + " pixels is bigger than Ragtime shows";
        return false;
    }
    bool decoded = opj_decode(codec.get(), stream.get(), j2k.get()) != 0;
    if (decoded) opj_end_decompress(codec.get(), stream.get());
    for (OPJ_UINT32 k = 0; k < std::min<OPJ_UINT32>(3, j2k->numcomps); k++)
    {
        if (j2k->comps[k].data == nullptr || j2k->comps[k].w == 0 || j2k->comps[k].h == 0)
        {
            if (error.empty()) error = "no picture in it yet";
            return false;
        }
    }

    bool colour = j2k->numcomps >= 3;
    out = picture::Image::blank(width, height, colour ? 3 : 1);
    std::vector<Component> comps;
    for (OPJ_UINT32 k = 0; k < (colour ? 3u : 1u); k++) comps.emplace_back(j2k->comps[k]);
    bool alpha = j2k->numcomps == 4 || j2k->numcomps == 2;
    std::unique_ptr<Component> a;
    if (alpha && j2k->comps[j2k->numcomps - 1].data != nullptr) a.reset(new Component(j2k->comps[j2k->numcomps - 1]));
    for (int y = 0; y < height; y++)
    {
        uint8_t* row = out.row(y);
        for (int x = 0; x < width; x++)
        {
            uint8_t* px = row + (size_t)x * (size_t)out.channels;
            int over = a ? a->at(x, y) : 255;
            for (size_t k = 0; k < comps.size(); k++)
                px[k] = (uint8_t)((comps[k].at(x, y) * over + 255 * (255 - over) + 127) / 255);
        }
    }
    error.clear();
    return true;
}

//-------------------------------------------------------------------------
// WebP and HEIF, from disk
//-------------------------------------------------------------------------

// RGBA laid over white, to RGB.
picture::Image overWhite(const uint8_t* rgba, int width, int height, size_t stride)
{
    picture::Image out = picture::Image::blank(width, height, 3);
    for (int y = 0; y < height; y++)
    {
        const uint8_t* in = rgba + (size_t)y * stride;
        uint8_t* o = out.row(y);
        for (int x = 0; x < width; x++, in += 4, o += 3)
        {
            int a = in[3];
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)((in[k] * a + 255 * (255 - a) + 127) / 255);
        }
    }
    return out;
}

bool isWebp(const uint8_t* b, size_t n)
{
    return n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0;
}

bool isHeif(const uint8_t* b, size_t n)
{
    if (n < 12 || std::memcmp(b + 4, "ftyp", 4) != 0) return false;
    static const char* brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif"};
    for (const char* brand : brands)
        if (std::memcmp(b + 8, brand, 4) == 0) return true;
    return false;
}

Load loadWebp(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error)
{
    int width = 0, height = 0;
    if (!WebPGetInfo(bytes, length, &width, &height))
    {
        error = "it isn't a WebP picture libwebp can read (an animated one, perhaps)";
        return Load::Failed;
    }
    if ((uint64_t)width * (uint64_t)height > MAX_LOAD_PIXELS)
    {
        error = "it is too big a picture";
        return Load::Failed;
    }
    uint8_t* rgba = WebPDecodeRGBA(bytes, length, &width, &height);
    if (rgba == nullptr)
    {
        error = "libwebp couldn't read it (an animated WebP, perhaps)";
        return Load::Failed;
    }
    out = overWhite(rgba, width, height, (size_t)width * 4);
    WebPFree(rgba);
    return Load::Done;
}

#if RAGTIME_HEIF
// libheif, once. A Linux build finds its HEVC decoder as a plugin in the
// folder it was built for; in the AppImage the plugin goes beside its own
// libraries (packaging/linux/make-appimage.sh), so libheif is pointed
// there unless the operator pointed it somewhere already.
void initHeif()
{
    static const bool done = []() {
#if defined(__linux__)
        const char* appDir = std::getenv("APPDIR");
        if (appDir != nullptr && std::getenv("LIBHEIF_PLUGIN_PATH") == nullptr)
        {
            // The plugin needs libde265, which the AppImage carries in its
            // own lib folder: loaded first, the plugin finds it loaded.
            dlopen((std::string(appDir) + "/usr/lib/libde265.so.0").c_str(), RTLD_NOW | RTLD_GLOBAL);
            setenv("LIBHEIF_PLUGIN_PATH", (std::string(appDir) + "/usr/lib/libheif/plugins").c_str(), 1);
        }
#endif
        heif_init(nullptr);
        return true;
    }();
    (void)done;
}

Load loadHeif(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error)
{
    initHeif();
    std::unique_ptr<heif_context, void (*)(heif_context*)> ctx(heif_context_alloc(), heif_context_free);
    if (!ctx) return Load::Failed;
    heif_error e = heif_context_read_from_memory_without_copy(ctx.get(), bytes, length, nullptr);
    if (e.code != heif_error_Ok)
    {
        error = std::string("libheif couldn't read it: ") + e.message;
        return Load::Failed;
    }
    heif_image_handle* h = nullptr;
    e = heif_context_get_primary_image_handle(ctx.get(), &h);
    if (e.code != heif_error_Ok)
    {
        error = std::string("libheif couldn't read it: ") + e.message;
        return Load::Failed;
    }
    std::unique_ptr<heif_image_handle, void (*)(const heif_image_handle*)> handle(h, heif_image_handle_release);
    if ((uint64_t)heif_image_handle_get_width(h) * (uint64_t)heif_image_handle_get_height(h) > MAX_LOAD_PIXELS)
    {
        error = "it is too big a picture";
        return Load::Failed;
    }
    // libheif turns and crops it as the file says while it decodes.
    heif_image* img = nullptr;
    e = heif_decode_image(h, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (e.code != heif_error_Ok)
    {
        error = std::string("libheif couldn't decode it: ") + e.message;
        return Load::Failed;
    }
    std::unique_ptr<heif_image, void (*)(const heif_image*)> image(img, heif_image_release);
    int stride = 0;
    const uint8_t* rgba = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    int width = heif_image_get_width(img, heif_channel_interleaved);
    int height = heif_image_get_height(img, heif_channel_interleaved);
    if (rgba == nullptr || width <= 0 || height <= 0 || stride < width * 4)
    {
        error = "libheif gave no pixels";
        return Load::Failed;
    }
    out = overWhite(rgba, width, height, (size_t)stride);
    return Load::Done;
}
#endif

} // namespace

bool available()
{
    return true;
}

bool heifAvailable()
{
#if RAGTIME_HEIF
    static const bool can = []() {
        initHeif();
        return heif_have_decoder_for_format(heif_compression_HEVC) != 0;
    }();
    return can;
#else
    return false;
#endif
}

std::string versions()
{
    int w = WebPGetDecoderVersion();
    std::string text = std::string("OpenJPEG ") + opj_version() + ", libwebp " + std::to_string((w >> 16) & 0xFF) + "."
                       + std::to_string((w >> 8) & 0xFF) + "." + std::to_string(w & 0xFF);
#if RAGTIME_HEIF
    text += std::string(", libheif ") + heif_get_version();
#endif
    return text;
}

Load load(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error)
{
    if (picture::isCodestream(bytes, length) || picture::isJp2File(bytes, length))
    {
        if (!decodeJ2k(bytes, length, picture::isJp2File(bytes, length), out, error, 1 << 15))
        {
            if (error.empty()) error = "OpenJPEG couldn't read it";
            return Load::Failed;
        }
        if ((uint64_t)out.width * (uint64_t)out.height > MAX_LOAD_PIXELS)
        {
            error = "it is too big a picture";
            return Load::Failed;
        }
        return Load::Done;
    }
    if (isWebp(bytes, length)) return loadWebp(bytes, length, out, error);
    if (isHeif(bytes, length))
    {
#if RAGTIME_HEIF
        return loadHeif(bytes, length, out, error);
#else
        error = "this Ragtime was built without HEIF";
        return Load::Failed;
#endif
    }
    return Load::NotMine;
}

bool encode(const picture::Image& image, uint64_t maxBytes, Coded& out, std::string& error)
{
    if (image.empty() || (image.channels != 1 && image.channels != 3))
    {
        error = "there is no picture to code";
        return false;
    }
    if (maxBytes < picture::MIN_PICTURE_BYTES)
    {
        error = "a picture needs " + std::to_string(picture::MIN_PICTURE_BYTES) + " bytes at least";
        return false;
    }
    const int resolutions = resolutionsFor(image.width, image.height);
    const double raw = (double)image.width * image.height * (image.channels == 1 ? 1 : 3);
    // OpenJPEG's rate control lands near, not on, what it is asked for:
    // ask, measure, and ask again, keeping the biggest that fits.
    double target = std::min((double)maxBytes, raw);
    std::vector<uint8_t> best, bytes;
    int bestLayers = 0;
    for (int attempt = 0; attempt < 7 && target >= picture::MIN_PICTURE_BYTES / 2.0; attempt++)
    {
        int layers = layersFor((uint64_t)target);
        std::string why;
        if (!encodeOnce(image, target, layers, resolutions, bytes, why))
        {
            error = why.empty() ? std::string("OpenJPEG couldn't code it") : why;
            return false;
        }
        double size = (double)bytes.size();
        if (size <= (double)maxBytes)
        {
            if (bytes.size() > best.size())
            {
                best = bytes;
                bestLayers = layers;
            }
            // Close enough, or as good as it gets: everything went in.
            if (size >= 0.97 * (double)maxBytes || target >= raw) break;
            target = std::min(raw, target * std::min(1.5, (double)maxBytes / size));
        }
        else
        {
            target *= (double)maxBytes / size * 0.98;
        }
    }
    if (best.empty())
    {
        error = "it can't be made as small as " + std::to_string(maxBytes) + " bytes";
        return false;
    }
    out.bytes = std::move(best);
    out.layers = bestLayers;
    out.resolutions = resolutions;
    return true;
}

bool decode(const uint8_t* bytes, size_t length, picture::Image& out, std::string& error, int maxSide)
{
    error.clear();
    if (!picture::isCodestream(bytes, length))
    {
        error = "it isn't a JPEG 2000 codestream";
        return false;
    }
    return decodeJ2k(bytes, length, false, out, error, maxSide);
}

} // namespace image
} // namespace ragtime
