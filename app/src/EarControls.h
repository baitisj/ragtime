#ifndef RAGTIME_EAR_CONTROLS_H
#define RAGTIME_EAR_CONTROLS_H

#include <functional>

#include <wx/control.h>
#include <wx/timer.h>

// The Ear's small controls (docs/STAGE1.md, "The Ear", step 3).

// A kitchen timer: a dial whose wedge is the time left, as Jeff asked for
// beside Undo (2026-10-26). It only draws; the owner says how much is left.
class ClockDial : public wxControl
{
public:
    ClockDial(wxWindow* parent, int sizeDip = 22);

    // left: 0 to 1, the share of the time still to run.
    void setLeft(double left);
    void setColour(const wxColour& colour);

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void paint();

    double left_ = 0;
    int size_;
    wxColour colour_;
};

// A preset button: a tap recalls, and pressing and holding stores, with a
// quick clock running down on the button as it is held (Jeff,
// 2026-10-26). Let go before the clock runs out and nothing is stored.
class PresetButton : public wxControl
{
public:
    static constexpr int HOLD_MS = 1000;

    PresetButton(wxWindow* parent, const wxString& name);

    // The second line: the edges or width it holds, or a dash.
    void setDetail(const wxString& detail);
    void onTap(std::function<void()> tap) { tap_ = std::move(tap); }
    void onStore(std::function<void()> store) { store_ = std::move(store); }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void paint();
    void tick();

    wxString name_;
    wxString detail_;
    std::function<void()> tap_;
    std::function<void()> store_;
    wxTimer timer_;
    long heldMs_ = -1;          // held this long; -1 not held
    long storedMs_ = -1;        // "Stored" shows this long; -1 not
    bool hover_ = false;
};

#endif
