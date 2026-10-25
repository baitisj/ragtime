#ifndef RAGTIME_CONNECTION_BAR_H
#define RAGTIME_CONNECTION_BAR_H

#include <wx/panel.h>

class TxLamp;
class ToggleSwitch;
class wxBoxSizer;
class wxCheckBox;
class wxGauge;
class wxStaticText;
class wxToggleButton;

// The bar across the top of the window (docs/MOCKUPS.md, "Connection
// bar"), left to right: my callsign, the group D2G, the radio's dial
// (while Ragtime reads it from rigctld), the 500 Hz switch,
// Accept sessions, Pause sending, Auto ALC with
// its level meter, and the TX light. The window wires the controls; the
// bar only lays them out and keeps their looks.
class ConnectionBar : public wxPanel
{
public:
    explicit ConnectionBar(wxWindow* parent);

    void setCallsign(const wxString& callsign);
    void setNarrow(bool narrow);
    void setPaused(bool paused);
    // The dial, "14.105 MHz · 20 m", in red with "Receive only" when
    // sending is held; empty hides it.
    void setDial(const wxString& text, const wxString& tip, bool held);

    ToggleSwitch* narrow = nullptr;  // on: 500 Hz modes only (BW500)
    wxCheckBox* acceptSessions = nullptr;
    wxToggleButton* pause = nullptr;
    wxCheckBox* autoAlc = nullptr;
    wxGauge* alcMeter = nullptr;
    wxStaticText* alcReading = nullptr;
    TxLamp* tx = nullptr;

private:
    void lightToggle(wxToggleButton* button);
    void arrange(int width);

    wxBoxSizer* left_ = nullptr;
    wxBoxSizer* right_ = nullptr;
    wxBoxSizer* first_ = nullptr;
    wxBoxSizer* second_ = nullptr;
    bool twoRows_ = false;

    wxStaticText* callsign_ = nullptr;
    wxStaticText* dial_ = nullptr;
    bool held_ = false;
};

#endif
