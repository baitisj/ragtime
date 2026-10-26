#ifndef RAGTIME_CONNECTION_BAR_H
#define RAGTIME_CONNECTION_BAR_H

#include <optional>

#include <wx/panel.h>

class SwrBar;
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
// its level meter (the rig's ALC, read through Hamlib while transmitting)
// with the SWR bar under it, and the TX light. The window wires the controls; the
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
    // The rig's ALC: the meter at percent (0 between bursts), the reading
    // beside it, red when warn, and their tooltip. Not readable greys them.
    void setAlc(bool readable, int percent, const wxString& text, const wxString& tip, bool warn);
    // The rig's SWR: the bar at swr (nullopt: empty), the reading beside
    // it, red when warn, the trip, and their tooltip.
    void setSwr(bool readable, std::optional<double> swr, const wxString& text, const wxString& tip, bool warn,
                bool tripped);

    ToggleSwitch* narrow = nullptr;  // on: 500 Hz modes only (BW500)
    wxCheckBox* acceptSessions = nullptr;
    wxToggleButton* pause = nullptr;
    wxCheckBox* autoAlc = nullptr;
    wxGauge* alcMeter = nullptr;
    wxStaticText* alcReading = nullptr;
    SwrBar* swrBar = nullptr;
    wxStaticText* swrReading = nullptr;
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
    bool alcWarn_ = false;
    bool swrWarn_ = false;
};

#endif
