#ifndef RAGTIME_UNPICK_CLICK_H
#define RAGTIME_UNPICK_CLICK_H

#include <functional>
#include <string>

#include <wx/gdicmn.h>
#include <wx/timer.h>

class wxMouseEvent;
class wxWindow;

// A click on the row already picked unpicks it (Jeff, 2026-11-02), so a
// station can be let go of and Globus with it. The unpick waits for the
// double-click time, so a double click on the picked row still opens a
// ragchew with the station rather than unpicking it and picking it again;
// the list's own double-click handler calls cancel().
class UnpickClick : public wxTimer
{
public:
    // The station on the row at a point of the window clicked, or "" off
    // the rows; the station picked, or "".
    using CallAtFn = std::function<std::string(wxWindow* clicked, const wxPoint& where)>;
    using CallFn = std::function<std::string()>;
    using UnpickFn = std::function<void()>;

    // Watches the left button on window. Called again, it watches another
    // window as well, with the same callbacks.
    void attach(wxWindow* window, CallAtFn callAt, CallFn picked, UnpickFn unpick);
    void cancel();

    void Notify() override;

private:
    void onDown(wxMouseEvent& event);

    CallAtFn callAt_;
    CallFn picked_;
    UnpickFn unpick_;
    std::string armed_;
};

#endif
