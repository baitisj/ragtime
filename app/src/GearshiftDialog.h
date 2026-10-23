#ifndef RAGTIME_GEARSHIFT_DIALOG_H
#define RAGTIME_GEARSHIFT_DIALOG_H

#include <array>
#include <functional>

#include <wx/dialog.h>

#include "ragtime/gearbox/Gearbox.h"

class wxCheckBox;
class wxListCtrl;
class wxListEvent;
class wxStaticText;
class wxToggleButton;
class SubmodeMap;

// The gearshift popup (docs/GEARBOX.md): the Data2G Submode Map with the
// gears on it, Width and Path chips, Auto, and the gear list. It opens from
// the Gear button by Send. Every change goes straight to the Gearshift it
// was given, and onChange runs after each, so the buttons can follow.
class GearshiftDialog : public wxDialog
{
public:
    GearshiftDialog(wxWindow* parent, ragtime::gearbox::Gearshift& shift,
                    std::function<void()> onChange);

private:
    void refresh();
    void changed();
    void loadFile();
    void onGearSelected(wxListEvent& event);

    ragtime::gearbox::Gearshift& shift_;
    std::function<void()> onChange_;
    bool refreshing_ = false;

    std::array<wxToggleButton*, 4> widthChips_ {};
    wxStaticText* widthNote_ = nullptr;
    std::array<wxToggleButton*, 4> pathChips_ {};
    wxCheckBox* auto_ = nullptr;
    SubmodeMap* map_ = nullptr;
    wxStaticText* inGear_ = nullptr;
    wxStaticText* snr_ = nullptr;
    wxListCtrl* gears_ = nullptr;
    wxStaticText* source_ = nullptr;
};

#endif
