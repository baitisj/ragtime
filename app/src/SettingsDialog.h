#ifndef RAGTIME_SETTINGS_DIALOG_H
#define RAGTIME_SETTINGS_DIALOG_H

#include <wx/dialog.h>

#include "Prefs.h"

class wxCheckBox;
class wxDirPickerCtrl;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

// Settings > Station: the callsign and locator Ragtime sends, where
// data2g-host is, where files go (the received folder, and the
// stations whose files are saved without asking), and whether our broadcast
// chat is sealed, and the radio: its dial read from rigctld, and receive
// only outside the US data segments. Small enough for a
// 1366x768 screen.
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
    wxDirPickerCtrl* received_ = nullptr;
    wxTextCtrl* autoAccept_ = nullptr;
    wxCheckBox* sealChat_ = nullptr;
    wxSpinCtrl* slowRepair_ = nullptr;
    wxCheckBox* readDial_ = nullptr;
    wxTextCtrl* rigHost_ = nullptr;
    wxSpinCtrl* rigPort_ = nullptr;
    wxCheckBox* usSegmentsOnly_ = nullptr;
    wxStaticText* problem_ = nullptr;
};

#endif
