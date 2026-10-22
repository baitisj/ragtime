#ifndef RAGTIME_THRESHOLDS_H
#define RAGTIME_THRESHOLDS_H

#include <wx/string.h>

#include "ragtime/gearbox/Submodes.h"

// Where the gearshift's thresholds come from. Ragtime carries none of
// Data2G's figures (docs/GEARBOX.md): the operator points it at a file,
// either the Submode Map's JSON or data2g-host's own
// data2g/codes_data/mode_thresholds.json.

// The file Ragtime looks for when none has been picked: submode_thresholds.json
// in the user data folder (~/.Ragtime on Linux, %APPDATA%\Ragtime on Windows).
wxString defaultThresholdsPath();

// The picked file (Preferences key /Gearshift/ThresholdsFile), else the
// default one if it exists. Empty when there is neither.
wxString thresholdsPath();
void rememberThresholdsPath(const wxString& path);

// Reads a thresholds file into *table (adding to what is there). False with
// a reason in *error.
bool loadThresholds(const wxString& path, ragtime::gearbox::SubmodeTable& table,
                    wxString& error);

#endif
