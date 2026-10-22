#ifndef RAGTIME_CONNECTION_BAR_H
#define RAGTIME_CONNECTION_BAR_H

#include <wx/panel.h>

class TxLamp;
class wxBoxSizer;
class wxButton;
class wxCheckBox;
class wxGauge;
class wxStaticText;
class wxToggleButton;

// The bar across the top of the window (docs/MOCKUPS.md, "Connection
// bar"), left to right: data2g-host's address and state, my callsign, the
// group D2G, the width button (it opens the gearshift) joined to the
// one-press 500 Hz button, Accept sessions, Pause sending, Auto ALC with
// its level meter, and the TX light. The window wires the controls; the
// bar only lays them out and keeps their looks.
class ConnectionBar : public wxPanel
{
public:
    explicit ConnectionBar(wxWindow* parent);

    void setHost(const wxString& address, const wxString& state, const wxString& tooltip);
    void setCallsign(const wxString& callsign);
    void setWidthLabel(const wxString& label);
    void setNarrow(bool narrow);
    void setPaused(bool paused);

    wxButton* width = nullptr;
    wxToggleButton* narrow = nullptr;
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

    wxStaticText* host_ = nullptr;
    wxStaticText* state_ = nullptr;
    wxStaticText* callsign_ = nullptr;
};

#endif
