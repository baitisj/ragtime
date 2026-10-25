#include "Report.h"

#include <cstdint>
#include <string>
#include <vector>

#include <wx/config.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/time.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include "FilesPage.h"
#include "ragtime/Version.h"

using ragtime::link::FlightRecorder;

namespace
{

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

uint64_t nowMs()
{
    return (uint64_t)wxGetUTCTimeMillis().GetValue();
}

// Every setting, one "path = value" line each, with secrets left out.
void listSettings(wxConfigBase& config, const wxString& path, std::string& out)
{
    wxString old = config.GetPath();
    config.SetPath(path.empty() ? wxString("/") : path);
    wxString name;
    long index = 0;
    for (bool more = config.GetFirstEntry(name, index); more; more = config.GetNextEntry(name, index))
    {
        std::string full(wxString(path + "/" + name).utf8_str());
        if (!ragtime::link::reportableSetting(full))
        {
            out += full + " = (left out)\n";
            continue;
        }
        wxString value;
        config.Read(name, &value);
        out += full + " = " + ragtime::link::escapeCommandLine(std::string(value.utf8_str())) + "\n";
    }
    std::vector<wxString> groups;
    for (bool more = config.GetFirstGroup(name, index); more; more = config.GetNextGroup(name, index))
        groups.push_back(name);
    for (const wxString& group : groups) listSettings(config, path + "/" + group, out);
    config.SetPath(old);
}

bool putText(wxZipOutputStream& zip, const char* name, const std::string& text)
{
    if (!zip.PutNextEntry(name)) return false;
    zip.Write(text.data(), text.size());
    return zip.IsOk();
}

const char* const README =
    "A Ragtime report, from Help > Save a report.\n"
    "\n"
    "report.txt    this Ragtime: version, system, data2g-host\n"
    "settings.txt  its settings, with anything named like a secret left out\n"
    "flight/       the flight recorder: what crossed the link to data2g-host\n"
    "              in the last 7 days, with UTC times to the millisecond.\n"
    "              > is to the host, < from it. Command lines and KISS frames\n"
    "              are whole (D2G broadcasts are public on the air anyway);\n"
    "              session bytes are counted, not kept.\n"
    "\n"
    "To play the host's side back to a Ragtime:\n"
    "  ragtime_fake_air --replay flight/ [--from 2026-10-25T16:20] [--speed 4]\n"
    "(docs/FLIGHT_RECORDER.md in the Ragtime repository).\n";

} // namespace

void saveReport(wxWindow* parent, FlightRecorder& recorder, const ReportFacts& facts)
{
    std::string call = facts.callsign.empty() ? std::string("ragtime") : facts.callsign;
    for (char& c : call)
    {
        if (c == '/' || c == '\\') c = '-';
    }
    wxString name = "ragtime-report-" + utf8(call) + "-"
                    + wxDateTime::UNow().Format("%Y%m%d-%H%M", wxDateTime::UTC) + "Z.zip";
    wxFileDialog dialog(parent, "Save a report", wxStandardPaths::Get().GetDocumentsDir(), name,
                        "Zip files (*.zip)|*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return;
    wxString path = dialog.GetPath();

    // A mark in the recording itself, for finding the moment.
    recorder.note("Help > Save a report", nowMs());

    std::string info;
    info += std::string("Ragtime ") + ragtime::versionString() + " report, saved "
            + ragtime::link::flightTime(nowMs()) + "\n";
    info += "System: " + std::string(wxGetOsDescription().utf8_str()) + ", "
            + std::string(wxGetCpuArchitectureName().utf8_str()) + ", "
            + std::string(wxGetLibraryVersionInfo().GetVersionString().utf8_str()) + "\n";
    info += "Callsign: " + (facts.callsign.empty() ? std::string("(none yet)") : facts.callsign) + "\n";
    info += "data2g-host: " + facts.host + ", " + facts.hostState + "\n";
    if (!facts.lastError.empty()) info += "Last connection error: " + facts.lastError + "\n";
    std::vector<std::string> names = recorder.fileNames();
    info += "Flight recorder: " + std::to_string(names.size()) + " file" + (names.size() == 1 ? "" : "s")
            + (names.empty() ? std::string() : ", " + names.front() + " to " + names.back()) + "\n";

    std::string settings;
    if (wxConfigBase* config = wxConfigBase::Get()) listSettings(*config, wxString(), settings);

    bool ok;
    {
        wxFFileOutputStream file(path);
        ok = file.IsOk();
        if (ok)
        {
            wxZipOutputStream zip(file, 9);
            ok = putText(zip, "README.txt", README) && putText(zip, "report.txt", info)
                 && putText(zip, "settings.txt", settings);
            for (const std::string& flight : names)
            {
                if (!ok) break;
                wxFileName source(utf8(recorder.folder()), utf8(flight));
                wxFFileInputStream in(source.GetFullPath());
                // A file pruned since the list was made is skipped.
                if (!in.IsOk()) continue;
                ok = zip.PutNextEntry("flight/" + utf8(flight));
                if (ok)
                {
                    zip.Write(in);
                    ok = zip.IsOk();
                }
            }
            ok = zip.Close() && ok;
            ok = file.Close() && ok;
        }
    }
    if (!ok)
    {
        wxMessageBox("Ragtime couldn't write " + path + ".", "Save a report", wxOK | wxICON_ERROR, parent);
        return;
    }

    wxULongLong size = wxFileName::GetSize(path);
    wxString kb;
    if (size != wxInvalidSize) kb = wxString::Format(" (%llu KB)", (unsigned long long)((size.GetValue() + 1023) / 1024));
    wxMessageDialog done(parent,
                         "Saved " + wxFileName(path).GetFullName() + kb
                             + ".\n\nSend it with what you saw and when (UTC, near enough). It holds the last 7 days "
                               "of the link to data2g-host and your settings, never a private key.",
                         "Save a report", wxYES_NO | wxICON_INFORMATION);
    done.SetYesNoLabels("Open folder", "Close");
    if (done.ShowModal() == wxID_YES) openWithDesktop(std::string(wxFileName(path).GetPath().utf8_str()));
}
