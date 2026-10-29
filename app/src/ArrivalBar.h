#ifndef RAGTIME_ARRIVAL_BAR_H
#define RAGTIME_ARRIVAL_BAR_H

#include <cstdint>
#include <vector>

#include <wx/control.h>

// Under the picture dialog's preview (Jeff, 2026-10-29): a progress bar of
// how much of the picture has come as the preview plays, the bursts' ends
// marked across it, the time it takes on an axis over it and a scale of
// the picture's size under it.
class ArrivalBar : public wxControl
{
public:
    ArrivalBar(wxWindow* parent, wxWindowID id = wxID_ANY);

    // The whole picture's bytes, where each burst ends and how long it
    // all takes (no bytes: no bar).
    void setWhole(uint64_t whole, const std::vector<uint64_t>& burstEnds, double seconds);
    // How much has come.
    void setDone(uint64_t done);

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent& event);

    uint64_t whole_ = 0;
    uint64_t done_ = 0;
    double seconds_ = 0.0;
    std::vector<uint64_t> ends_;
};

#endif
