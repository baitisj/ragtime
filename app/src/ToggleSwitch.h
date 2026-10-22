#ifndef RAGTIME_TOGGLE_SWITCH_H
#define RAGTIME_TOGGLE_SWITCH_H

#include <wx/control.h>

// A slide switch with its label beside it, as phones draw them: the knob
// slides right and the track turns blue when on. A click or Space flips it
// and sends wxEVT_TOGGLEBUTTON (IsChecked() is the new state), as a
// wxToggleButton would; SetValue() sends nothing.
class ToggleSwitch : public wxControl
{
public:
    ToggleSwitch(wxWindow* parent, wxWindowID id, const wxString& label);

    bool GetValue() const { return on_; }
    void SetValue(bool on);

    bool AcceptsFocus() const override { return IsEnabled(); }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void flip();

    bool on_ = false;
    bool hover_ = false;
};

#endif
