#ifndef RAGTIME_TX_LAMP_H
#define RAGTIME_TX_LAMP_H

#include <wx/control.h>

// The TX light at the right end of the connection bar: "TX Off" in grey,
// or "TX" lit red while data2g-host reports PTT on. The word changes with
// the colour, so it never relies on colour alone.
class TxLamp : public wxControl
{
public:
    explicit TxLamp(wxWindow* parent, wxWindowID id = wxID_ANY);

    void setOn(bool on);
    bool isOn() const { return on_; }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);

    bool on_ = false;
};

#endif
