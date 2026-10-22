#ifndef RAGTIME_GLOBUS_TOGGLE_H
#define RAGTIME_GLOBUS_TOGGLE_H

#include <wx/control.h>

// The Globus toggle: a small globe and the word "Globus", blue when the
// globe is showing. It sits in the globe pane's header, and package E moves
// it to the right end of the tab strip, unlit, when the globe is hidden
// (docs/MOCKUPS.md). Clicked, or Space or Enter with the focus on it, it
// flips and sends wxEVT_TOGGLEBUTTON with IsChecked() saying which way.
class GlobusToggle : public wxControl
{
public:
    explicit GlobusToggle(wxWindow* parent, wxWindowID id = wxID_ANY);

    bool isOn() const { return on_; }
    // Sets it without sending an event.
    void setOn(bool on);

    bool AcceptsFocus() const override { return true; }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void flip();

    bool on_ = true;
};

#endif
