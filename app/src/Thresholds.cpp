#include "Thresholds.h"

#include <string>

#include <wx/config.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

namespace
{

// A thresholds file is a few tens of kilobytes; anything far bigger is not one.
constexpr wxFileOffset MAX_FILE_BYTES = 4 * 1024 * 1024;

const char* CONFIG_KEY = "/Gearshift/ThresholdsFile";

} // namespace

wxString defaultThresholdsPath()
{
    return wxFileName(wxStandardPaths::Get().GetUserDataDir(), "submode_thresholds.json")
        .GetFullPath();
}

wxString thresholdsPath()
{
    const wxString picked = wxConfigBase::Get()->Read(CONFIG_KEY, wxString());
    if (!picked.empty())
        return picked;
    const wxString fallback = defaultThresholdsPath();
    return wxFileName::FileExists(fallback) ? fallback : wxString();
}

void rememberThresholdsPath(const wxString& path)
{
    wxConfigBase::Get()->Write(CONFIG_KEY, path);
    wxConfigBase::Get()->Flush();
}

bool loadThresholds(const wxString& path, ragtime::gearbox::SubmodeTable& table,
                    wxString& error)
{
    wxFile file;
    if (!wxFileName::FileExists(path) || !file.Open(path))
    {
        error = "can't open " + path;
        return false;
    }
    const wxFileOffset length = file.Length();
    if (length <= 0 || length > MAX_FILE_BYTES)
    {
        error = path + " is empty or too big to be a thresholds file";
        return false;
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    if (file.Read(&text[0], static_cast<std::size_t>(length)) != length)
    {
        error = "can't read " + path;
        return false;
    }

    std::string why;
    if (!table.parseJson(text, &why))
    {
        error = wxFileName(path).GetFullName() + ": " + wxString::FromUTF8(why);
        return false;
    }
    return true;
}
