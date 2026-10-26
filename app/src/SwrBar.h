#ifndef RAGTIME_SWR_BAR_H
#define RAGTIME_SWR_BAR_H

#include <functional>
#include <optional>

#include <wx/control.h>

// The SWR bargraph under the ALC meter on the connection bar (Jeff,
// 2026-10-26): the rig's SWR, read through Hamlib while transmitting, on
// the reflection coefficient's scale (radio::swrScale), with small marks
// at 1, 1.5, 2 and 3. The bar is green under 1.5, yellow to 2, orange to 3
// and red over 3 (radio::swrBand). After an SWR trip it is outlined in
// red, and a click asks to clear the trip.
class SwrBar : public wxControl
{
public:
    explicit SwrBar(wxWindow* parent, wxWindowID id = wxID_ANY);

    // The SWR to show; nullopt for an empty bar.
    void setSwr(std::optional<double> swr);
    void setTripped(bool tripped);
    // A click while tripped.
    void onClear(std::function<void()> clear) { clear_ = std::move(clear); }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);

    std::optional<double> swr_;
    bool tripped_ = false;
    std::function<void()> clear_;
};

#endif
