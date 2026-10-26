#ifndef RAGTIME_SETTINGS_DIALOG_H
#define RAGTIME_SETTINGS_DIALOG_H

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "Prefs.h"
#include "ragtime/rig/HamlibRig.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxComboBox;
class wxDirPickerCtrl;
class wxNotebook;
class wxRadioButton;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

// Settings, in two tabs.
//
// Station: the callsign and locator Ragtime sends, where data2g-host is,
// where files go (the received folder, and the stations whose files are
// saved without asking), whether our broadcast chat is sealed, and how
// repairs go (the slowest gear, the caps).
//
// Rig: how Ragtime knows the radio (radio::RigSettings). No rig; the dial
// read from a rigctld already running; or the rig opened with Hamlib, any
// rig Hamlib knows, with its port, the serial settings and the backend's
// own, a Test button that reads the dial, and "Key the rig when
// data2g-host keys" (off by default). Then receive only outside the US
// data segments.
//
// Small enough for a 1366x768 screen.
class SettingsDialog : public wxDialog
{
public:
    enum class Page
    {
        Station,
        Rig,
    };

    // gears: the gearshift's ladder, most robust first, as (Data2G's mode,
    // what to call it), for the slowest repair gear. openRig: how the rig
    // Ragtime has open is doing, if it is open with these settings (its
    // port is taken, so Test reports it instead); empty otherwise.
    SettingsDialog(wxWindow* parent, const StationPrefs& prefs,
                   const std::vector<std::pair<std::string, std::string>>& gears,
                   std::function<std::string(const ragtime::radio::RigSettings&)> openRig, Page page);
    ~SettingsDialog() override;

    StationPrefs prefs() const;

private:
    wxWindow* stationPage(wxNotebook* book, const StationPrefs& prefs,
                          const std::vector<std::pair<std::string, std::string>>& gears);
    wxWindow* rigPage(wxNotebook* book, const StationPrefs& prefs);
    ragtime::radio::RigSettings rig() const;
    // The model picked, if Hamlib knows it.
    const ragtime::rig::Model* pickedModel() const;
    void fillModels(int pick);
    void modelPicked();
    void test();
    void onTestTimer(wxTimerEvent& event);
    bool TransferDataFromWindow() override;
    void check();

    // Station.
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

    // Rig.
    wxRadioButton* noRig_ = nullptr;
    wxRadioButton* useRigctld_ = nullptr;
    wxRadioButton* useHamlib_ = nullptr;
    wxTextCtrl* rigctldHost_ = nullptr;
    wxSpinCtrl* rigctldPort_ = nullptr;
    wxChoice* maker_ = nullptr;
    wxChoice* model_ = nullptr;
    std::vector<ragtime::rig::Model> models_;      // Hamlib's, by maker and name
    std::vector<std::string> makers_;              // maker_'s, in order
    std::vector<size_t> shown_;                    // models_ indices in model_, in order
    int unknownModel_ = 0;                         // a saved model this Hamlib doesn't know
    wxComboBox* port_ = nullptr;
    wxChoice* baud_ = nullptr;
    wxChoice* dataBits_ = nullptr;
    wxChoice* stopBits_ = nullptr;
    wxChoice* parity_ = nullptr;
    wxChoice* handshake_ = nullptr;
    wxChoice* dtr_ = nullptr;
    wxChoice* rts_ = nullptr;
    std::vector<wxWindow*> serial_;                // the serial settings and their labels
    wxTextCtrl* backend_ = nullptr;
    wxSpinCtrl* poll_ = nullptr;
    wxButton* testButton_ = nullptr;
    wxStaticText* testResult_ = nullptr;
    wxCheckBox* keyRig_ = nullptr;
    wxChoice* ptt_ = nullptr;
    wxComboBox* pttPort_ = nullptr;
    std::vector<wxWindow*> keying_;                // what keyRig_ enables
    wxCheckBox* usSegmentsOnly_ = nullptr;

    wxStaticText* problem_ = nullptr;

    std::function<std::string(const ragtime::radio::RigSettings&)> openRig_;
    std::unique_ptr<ragtime::rig::HamlibRig> testRig_;
    wxTimer testTimer_;
    int testTicks_ = 0;
};

#endif
