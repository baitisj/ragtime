#ifndef RAGTIME_PICTURES_H
#define RAGTIME_PICTURES_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <wx/image.h>
#include <wx/string.h>

#include "ragtime/picture/Picture.h"

class wxWindow;

// Inline pictures in the window (docs/STAGE1.md, "Inline pictures"): a
// picture read from disk in any format Ragtime knows, and the shelf of
// pictures shown among the messages, each decoded from its bytes as they
// stand and again only when more have come.

// Reads a picture to send: JPEG 2000, WebP and HEIF through the image
// library, everything else wxImage reads (JPEG, PNG, GIF, BMP, TIFF),
// turned the way up its EXIF says, transparency over white. False, with
// the reason, if it can't be.
bool loadPicture(const std::string& path, ragtime::picture::Image& out, std::string& error);

wxImage toWxImage(const ragtime::picture::Image& image);

// The file dialog's wildcard for pictures Ragtime reads.
wxString pictureWildcard();

class PictureShelf
{
public:
    // A picture as it stands: decoded from bytes of its size.
    struct Shelved
    {
        wxImage image;          // empty until something decodes
        uint64_t bytes = 0;     // decoded from this many
        uint64_t size = 0;      // of this many in all
        int width = 0;          // the codestream's own size
        int height = 0;
        int version = 0;        // moves each time it is decoded anew
    };

    // A picture's bytes as they stand (key: "b<id>" for a broadcast file,
    // "s<id>" for a session transfer, "p<path>" for one on disk): decoded
    // again if more have come, at most once a DECODE_MS unless it is whole
    // or final (it has ended, whole or not, and won't grow). True if what
    // shows changed.
    static constexpr uint64_t DECODE_MS = 1000;
    bool update(const std::string& key, const std::vector<uint8_t>& bytes, uint64_t size, uint64_t nowMs,
                bool final = false);
    // Whole or final, and shown: nothing more to read for it.
    bool settled(const std::string& key) const;
    const Shelved* find(const std::string& key) const;
    void forget(const std::string& key);

    // The picture among the messages: an <img> at most maxDip on its longer
    // side, linked to href; "" while nothing has decoded.
    wxString thumbHtml(const std::string& key, const wxWindow* window, int maxDip, const wxString& href);

private:
    struct Entry
    {
        Shelved shelved;
        uint64_t decodedMs = 0;
        bool settled = false;
        std::string memoryName;     // its thumbnail in wxHTML's memory files
        int thumbVersion = -1;
        int thumbPx = 0;
    };
    std::map<std::string, Entry> entries_;
    int nextName_ = 1;
};

// The red stop sign for a picture coming in a session, as an <img> for a
// wxHTML row (drawn once into wxHTML's memory files).
wxString stopSignHtml(const wxWindow* window, int sizeDip);

#endif
