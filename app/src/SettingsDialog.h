#ifndef RAGTIME_SETTINGS_DIALOG_H
#define RAGTIME_SETTINGS_DIALOG_H

#include <string>
#include <utility>
#include <vector>

#include <wx/dialog.h>

#include "Prefs.h"

class wxCheckBox;
class wxChoice;
class wxDirPickerCtrl;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

// Settings > Station: the callsign and locator Ragtime sends, where
// data2g-host is, where files go (the received folder, and the
// stations whose files are saved without asking), and whether our broadcast
// chat is sealed, how repairs go (the slowest gear, the caps), and the
// radio: its dial read from rigctld, and receive
// only outside the US data segments. Small enough for a
// 1366x768 screen.
class SettingsDialog : public wxDialog
{
public:
    // gears: the gearshift's ladder, most robust first, as (Data2G's mode,
    // what to call it), for the slowest repair gear.
    SettingsDialog(wxWindow* parent, const StationPrefs& prefs,
                   const std::vector<std::pair<std::string, std::string>>& gears);

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
    wxChoice* repairFloor_ = nullptr;
    std::vector<std::string> floorModes_;   // repairFloor_'s, in order; "" first
    wxCheckBox* repairCap_ = nullptr;
    wxSpinCtrl* repairCapMinutes_ = nullptr;
    wxCheckBox* readDial_ = nullptr;
    wxTextCtrl* rigHost_ = nullptr;
    wxSpinCtrl* rigPort_ = nullptr;
    wxCheckBox* usSegmentsOnly_ = nullptr;
    wxStaticText* problem_ = nullptr;
};

#endif
