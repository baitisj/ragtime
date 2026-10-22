#include "Thresholds.h"

#include <string>

#include <wx/config.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include "Prefs.h"

namespace
{

// A thresholds file is a few tens of kilobytes; anything far bigger is not one.
constexpr wxFileOffset MAX_FILE_BYTES = 4 * 1024 * 1024;

const char* CONFIG_KEY = "/Gearshift/ThresholdsFile";

// The files the last loadAllThresholds() used, for the popup.
wxArrayString lastSources;

} // namespace

wxString defaultThresholdsPath()
{
    return wxFileName(wxString::FromUTF8(appDataDir()), "submode_thresholds.json").GetFullPath();
}

wxString thresholdsPath()
{
    const wxString picked = wxConfigBase::Get()->Read(CONFIG_KEY, wxString());
    if (!picked.empty())
        return picked;
    const wxString fallback = defaultThresholdsPath();
    return wxFileName::FileExists(fallback) ? fallback : wxString();
}

wxArrayString data2gSearchFolders()
{
    wxArrayString folders;
    wxString home;
    if (wxGetEnv("DATA2G_HOME", &home) && !home.empty())
        folders.Add(home);
    const wxString user = wxGetHomeDir();
    for (const char* parent : {"", "src", "git", "code", "projects", "Projects", "repos", "dev"})
    {
        for (const char* name : {"Data2G", "data2g"})
        {
            wxFileName folder = wxFileName::DirName(user);
            if (parent[0] != '\0')
                folder.AppendDir(parent);
            folder.AppendDir(name);
            folders.Add(folder.GetPath());
        }
    }
    return folders;
}

wxString findData2GThresholds()
{
    for (const wxString& folder : data2gSearchFolders())
    {
        wxFileName file = wxFileName::DirName(folder);
        file.AppendDir("data2g");
        file.AppendDir("codes_data");
        file.SetFullName("mode_thresholds.json");
        if (file.FileExists())
            return file.GetFullPath();
    }
    return wxString();
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

ragtime::gearbox::SubmodeTable loadAllThresholds(wxArrayString& sources, wxString& errors)
{
    ragtime::gearbox::SubmodeTable table;
    sources.Clear();
    errors.clear();
    for (const wxString& path : {findData2GThresholds(), thresholdsPath()})
    {
        if (path.empty() || sources.Index(path) != wxNOT_FOUND)
            continue;
        wxString error;
        if (loadThresholds(path, table, error))
            sources.Add(path);
        else
            errors += (errors.empty() ? "" : "\n") + error;
    }
    lastSources = sources;
    return table;
}

wxArrayString loadedThresholdSources()
{
    return lastSources;
}
