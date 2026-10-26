#ifndef RAGTIME_GEARSHIFT_DIALOG_H
#define RAGTIME_GEARSHIFT_DIALOG_H

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "ragtime/gearbox/Gearbox.h"

class wxCheckBox;
class wxChoice;
class wxListCtrl;
class wxListEvent;
class wxStaticText;
class wxToggleButton;
class SubmodeMap;

// The band's noise from the rig's S-meter (RigControl), for the map's QRN
// line and its S-meter axis: whether the rig reports one, and the noise in
// dB over S9 once read.
struct BandNoise
{
    bool reads = false;
    std::optional<double> dbOverS9;
};

// The gearshift popup (docs/GEARBOX.md): the Data2G Submode Map with the
// gears on it, Width and Path chips, Auto, and the gear list. It opens from
// the Gear button by Send. Every change goes straight to the Gearshift it
// was given, and onChange runs after each, so the buttons can follow.
// noise, when given, is asked once a second.
class GearshiftDialog : public wxDialog
{
public:
    GearshiftDialog(wxWindow* parent, ragtime::gearbox::Gearshift& shift,
                    std::function<void()> onChange, std::function<BandNoise()> noise = nullptr);

private:
    void refresh();
    void changed();
    void loadFile();
    void onGearSelected(wxListEvent& event);

    ragtime::gearbox::Gearshift& shift_;
    std::function<void()> onChange_;
    std::function<BandNoise()> noise_;
    wxTimer noiseTimer_;
    bool refreshing_ = false;

    std::array<wxToggleButton*, 4> widthChips_ {};
    wxStaticText* widthNote_ = nullptr;
    std::array<wxToggleButton*, 4> pathChips_ {};
    wxCheckBox* auto_ = nullptr;
    SubmodeMap* map_ = nullptr;
    wxStaticText* inGear_ = nullptr;
    wxStaticText* snr_ = nullptr;
    wxListCtrl* gears_ = nullptr;
    wxChoice* anyMode_ = nullptr;
    std::vector<std::string> anyModes_;  // anyMode_'s modes, by row
    wxStaticText* source_ = nullptr;
};

#endif
