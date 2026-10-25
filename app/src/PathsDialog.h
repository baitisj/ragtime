#ifndef RAGTIME_PATHS_DIALOG_H
#define RAGTIME_PATHS_DIALOG_H

#include <cstdint>
#include <string>

#include <wx/dialog.h>

#include "ragtime/radio/PathLog.h"

// "Paths heard from KC2G" (the station's right-click menu): its 24-hour
// strip by band from the path log, a line in words for each band, and a
// button that opens the log itself. Fits a 1366x768 screen.
class PathsDialog : public wxDialog
{
public:
    PathsDialog(wxWindow* parent, const ragtime::radio::PathStrip& strip, uint64_t nowMs,
                const std::string& logPath);
};

#endif
