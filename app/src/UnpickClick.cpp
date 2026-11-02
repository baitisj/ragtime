#include "UnpickClick.h"

#include <utility>

#include <wx/event.h>
#include <wx/settings.h>
#include <wx/window.h>

namespace
{

// When the system doesn't say, GTK's default.
constexpr int DEFAULT_DCLICK_MS = 400;

} // namespace

void UnpickClick::attach(wxWindow* window, CallAtFn callAt, CallFn picked, UnpickFn unpick)
{
    callAt_ = std::move(callAt);
    picked_ = std::move(picked);
    unpick_ = std::move(unpick);
    window->Bind(wxEVT_LEFT_DOWN, &UnpickClick::onDown, this);
}

void UnpickClick::cancel()
{
    Stop();
    armed_.clear();
}

void UnpickClick::onDown(wxMouseEvent& event)
{
    wxWindow* clicked = wxDynamicCast(event.GetEventObject(), wxWindow);
    std::string call = callAt_ && clicked != nullptr ? callAt_(clicked, event.GetPosition()) : std::string();
    if (call.empty() || !picked_ || call != picked_())
    {
        cancel();
        event.Skip();
        return;
    }
    // Taken here, so the list doesn't pick the row again; the focus still
    // goes to it.
    clicked->SetFocus();
    armed_ = call;
    int wait = wxSystemSettings::GetMetric(wxSYS_DCLICK_MSEC);
    StartOnce(wait > 0 ? wait : DEFAULT_DCLICK_MS);
}

void UnpickClick::Notify()
{
    std::string call = armed_;
    armed_.clear();
    // Still picked (a key may have picked another meanwhile), wherever a
    // refresh has moved its row.
    if (!call.empty() && picked_ && picked_() == call && unpick_) unpick_();
}
