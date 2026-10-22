#ifndef RAGTIME_SETTINGS_DIALOG_H
#define RAGTIME_SETTINGS_DIALOG_H

#include <wx/dialog.h>

#include "Prefs.h"

class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

// Settings > Station: the callsign and locator Ragtime sends, and where
// data2g-host is. Small enough for a 1366x768 screen.
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent, const StationPrefs& prefs);

    StationPrefs prefs() const;

private:
    bool TransferDataFromWindow() override;
    void check();

    wxTextCtrl* callsign_ = nullptr;
    wxTextCtrl* locator_ = nullptr;
    wxTextCtrl* host_ = nullptr;
    wxSpinCtrl* kissPort_ = nullptr;
    wxSpinCtrl* commandPort_ = nullptr;
    wxStaticText* dataPort_ = nullptr;
    wxStaticText* problem_ = nullptr;
};

#endif
