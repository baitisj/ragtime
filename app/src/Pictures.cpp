#include "Pictures.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/filesys.h>
#include <wx/font.h>
#include <wx/fs_mem.h>
#include <wx/graphics.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/window.h>

#include "ragtime/image/Codec.h"

using ragtime::picture::Image;

namespace
{

void addMemoryHandlers()
{
    // The seal badges and bells may have added them already: wx keeps one
    // of each.
    if (!wxFileSystem::HasHandlerForPath("memory:x")) wxFileSystem::AddHandler(new wxMemoryFSHandler);
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
}

// The handlers wxImage reads pictures from disk with, each added once (wx
// keeps one of each, and says so when given another).
void addReadHandlers()
{
    if (wxImage::FindHandler(wxBITMAP_TYPE_JPEG) == nullptr) wxImage::AddHandler(new wxJPEGHandler);
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
    if (wxImage::FindHandler(wxBITMAP_TYPE_GIF) == nullptr) wxImage::AddHandler(new wxGIFHandler);
    if (wxImage::FindHandler(wxBITMAP_TYPE_TIFF) == nullptr) wxImage::AddHandler(new wxTIFFHandler);
}

bool readAll(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// A wxImage as an Image: RGB, any alpha laid over white.
Image fromWxImage(const wxImage& image)
{
    Image out = Image::blank(image.GetWidth(), image.GetHeight(), 3);
    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    const size_t n = (size_t)out.width * (size_t)out.height;
    for (size_t i = 0; i < n; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            int v = rgb[i * 3 + c];
            if (alpha != nullptr) v = (v * alpha[i] + 255 * (255 - alpha[i]) + 127) / 255;
            out.pixels[i * 3 + c] = (uint8_t)v;
        }
    }
    return out;
}

} // namespace

bool loadPicture(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!readAll(path, bytes) || bytes.empty())
    {
        error = "It can't be read.";
        return false;
    }
    ragtime::image::Load loaded = ragtime::image::load(bytes.data(), bytes.size(), out, error);
    if (loaded == ragtime::image::Load::Done) return true;
    if (loaded == ragtime::image::Load::Failed) return false;

    addReadHandlers();
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage image;
    {
        wxLogNull quiet; // wx's own complaint would pop up a box
        if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY) || !image.IsOk())
        {
            error = "Ragtime doesn't know this kind of picture.";
            return false;
        }
    }
    if (image.HasMask() && !image.HasAlpha()) image.InitAlpha();
    out = ragtime::picture::orient(fromWxImage(image),
                                   ragtime::picture::jpegOrientation(bytes.data(), bytes.size()));
    return true;
}

wxImage toWxImage(const Image& image)
{
    if (image.empty()) return wxImage();
    wxImage out(image.width, image.height, false);
    unsigned char* rgb = out.GetData();
    const size_t n = (size_t)image.width * (size_t)image.height;
    for (size_t i = 0; i < n; i++)
    {
        for (int c = 0; c < 3; c++)
            rgb[i * 3 + c] = image.channels == 1 ? image.pixels[i] : image.pixels[i * image.channels + c];
    }
    return out;
}

wxString pictureWildcard()
{
    wxString kinds = "*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.tif;*.tiff;*.j2k;*.j2c;*.jpc;*.jp2";
    if (ragtime::image::available()) kinds += ";*.webp";
    if (ragtime::image::heifAvailable()) kinds += ";*.heic;*.heif;*.avif";
    // GTK matches the case given, and cameras shout.
    wxString both = kinds + ";" + kinds.Upper();
    return "Pictures (" + kinds + ")|" + both + "|All files (*)|*";
}

//-------------------------------------------------------------------------
// PictureShelf
//-------------------------------------------------------------------------

bool PictureShelf::update(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size,
                          uint64_t nowMs, bool final)
{
    Entry& e = entries_[key];
    Shelved& s = e.shelved;
    if (e.settled) return false;
    s.size = size;
    const bool whole = size > 0 && bytes.size() >= size;
    if (bytes.size() <= s.bytes)
    {
        e.settled = (final || whole) && s.version > 0;
        return false;
    }
    if (!whole && !final && s.version > 0 && nowMs < e.decodedMs + DECODE_MS) return false;

    if (s.width == 0)
    {
        int comps = 0;
        ragtime::picture::codestreamSize(bytes.data(), bytes.size(), s.width, s.height, comps);
    }
    Image image;
    std::string error;
    if (!ragtime::image::decode(bytes.data(), bytes.size(), image, error))
    {
        // Not a picture after all, or not yet: an ended one isn't read again.
        e.settled = whole || final;
        e.decodedMs = nowMs;
        return false;
    }
    s.image = toWxImage(image);
    s.bytes = bytes.size();
    s.version++;
    e.decodedMs = nowMs;
    e.settled = whole || final;
    return true;
}

bool PictureShelf::settled(const std::string& key) const
{
    auto it = entries_.find(key);
    return it != entries_.end() && it->second.settled;
}

const PictureShelf::Shelved* PictureShelf::find(const std::string& key) const
{
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.shelved.image.IsOk()) return nullptr;
    return &it->second.shelved;
}

void PictureShelf::forget(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    if (!it->second.memoryName.empty()) wxMemoryFSHandler::RemoveFile(it->second.memoryName);
    entries_.erase(it);
}

wxString PictureShelf::thumbHtml(const std::string& key, const wxWindow* window, int maxDip,
                                 const wxString& href)
{
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.shelved.image.IsOk()) return wxString();
    Entry& e = it->second;
    const wxImage& full = e.shelved.image;
    int w = 0, h = 0;
    ragtime::picture::fitWithin(full.GetWidth(), full.GetHeight(), window->FromDIP(maxDip), w, h);

    if (e.thumbVersion != e.shelved.version || e.thumbPx != w)
    {
        addMemoryHandlers();
        if (!e.memoryName.empty()) wxMemoryFSHandler::RemoveFile(e.memoryName);
        // A new name each time, so wxHTML never shows one it cached.
        e.memoryName = wxString::Format("ragtime-picture-%d.png", nextName_++).ToStdString();
        wxImage thumb = full;
        if (w != full.GetWidth() || h != full.GetHeight()) thumb.Rescale(w, h, wxIMAGE_QUALITY_HIGH);
        wxMemoryFSHandler::AddFile(e.memoryName, thumb, wxBITMAP_TYPE_PNG);
        e.thumbVersion = e.shelved.version;
        e.thumbPx = w;
    }
    wxString img = wxString::Format("<img src=\"memory:%s\" width=\"%d\" height=\"%d\">",
                                    wxString(e.memoryName), w, h);
    if (href.empty()) return img;
    return "<a href=\"" + href + "\">" + img + "</a>";
}

//-------------------------------------------------------------------------
// The stop sign
//-------------------------------------------------------------------------

namespace
{

void drawStopSign(wxGraphicsContext& gc, double size)
{
    // An octagon, white-rimmed, with STOP across it.
    const double c = size / 2.0;
    const double pi = 3.14159265358979323846;
    auto octagon = [&](double r) {
        wxGraphicsPath path = gc.CreatePath();
        for (int i = 0; i < 8; i++)
        {
            const double a = pi / 8.0 + i * pi / 4.0;
            const double x = c + r * std::cos(a);
            const double y = c + r * std::sin(a);
            if (i == 0)
                path.MoveToPoint(x, y);
            else
                path.AddLineToPoint(x, y);
        }
        path.CloseSubpath();
        return path;
    };
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    gc.FillPath(octagon(size * 0.5));
    gc.SetBrush(wxBrush(wxColour(200, 16, 30)));
    gc.FillPath(octagon(size * 0.44));

    wxFont font(wxFontInfo(wxSize(0, std::max(4, (int)std::lround(size * 0.26)))).Bold());
    gc.SetFont(font, wxColour(255, 255, 255));
    double tw = 0, th = 0;
    gc.GetTextExtent("STOP", &tw, &th);
    if (tw > size * 0.8)
    {
        font.SetPixelSize(wxSize(0, std::max(4, (int)std::lround(size * 0.26 * size * 0.8 / tw))));
        gc.SetFont(font, wxColour(255, 255, 255));
        gc.GetTextExtent("STOP", &tw, &th);
    }
    gc.DrawText("STOP", c - tw / 2.0, c - th / 2.0);
}

} // namespace

wxString stopSignHtml(const wxWindow* window, int sizeDip)
{
    const int px = window->FromDIP(sizeDip);
    wxString name = wxString::Format("ragtime-stop-%d.png", px);
    static std::set<wxString> made;
    if (made.count(name) == 0)
    {
        addMemoryHandlers();
        wxImage image(px, px);
        image.InitAlpha();
        unsigned char* alpha = image.GetAlpha();
        for (int i = 0; i < px * px; i++) alpha[i] = 0;
        {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
            if (!gc) return wxString();
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            drawStopSign(*gc, px);
        }
        wxMemoryFSHandler::AddFile(name, image, wxBITMAP_TYPE_PNG);
        made.insert(name);
    }
    wxString side = wxString::Format("%d", px);
    return "<img src=\"memory:" + name + "\" width=\"" + side + "\" height=\"" + side +
           "\" alt=\"Stop\" title=\"Stop: I have enough\">";
}
