#ifndef RAGTIME_SESSION_PAGE_H
#define RAGTIME_SESSION_PAGE_H

#include <cstdint>
#include <functional>
#include <string>

#include <wx/panel.h>

#include "ragtime/session/SessionChat.h"

class wxBoxSizer;
class wxButton;
class wxStaticText;
class wxTextCtrl;
class SessionMessages;

// The Session tab (docs/PLAN.md, "Sometimes we ragchew"): the status strip
// across the top with the one button the moment needs (Cancel while
// calling, Leave in a session, Leave now while leaving, Call again once it
// has ended); the session's messages, each of ours Sent, then Delivered
// when the far end's modem has acknowledged it; and the line to type in.
// With no session up the tab is greyed: the last session's messages stay
// readable, and nothing can be typed.
class SessionPage : public wxPanel
{
public:
    SessionPage(wxWindow* parent, const ragtime::session::SessionChat& session);

    // Send pressed: true when the text went, and the line is cleared.
    void onSend(std::function<bool(const std::string& typed)> fn) { onSend_ = std::move(fn); }
    void onCancel(std::function<void()> fn) { onCancel_ = std::move(fn); }
    void onLeave(std::function<void()> fn) { onLeave_ = std::move(fn); }
    void onLeaveNow(std::function<void()> fn) { onLeaveNow_ = std::move(fn); }
    void onCallAgain(std::function<void()> fn) { onCallAgain_ = std::move(fn); }

    // Redraws the strip every second, and the messages when they change.
    void refresh(uint64_t nowMs, long utcOffsetS);
    void setPaused(bool paused);
    void focusCompose();

private:
    friend class SessionMessages;

    void send();
    void showMenu(int row);

    const ragtime::session::SessionChat& session_;
    wxPanel* strip_ = nullptr;
    wxStaticText* stripText_ = nullptr;
    wxButton* cancel_ = nullptr;
    wxButton* leave_ = nullptr;
    wxButton* leaveNow_ = nullptr;
    wxButton* callAgain_ = nullptr;
    SessionMessages* list_ = nullptr;
    wxStaticText* paused_ = nullptr;
    wxTextCtrl* compose_ = nullptr;
    wxButton* send_ = nullptr;
    uint64_t shownVersion_ = ~0ull;
    std::string shownStrip_;
    bool shownAmber_ = false;
    int shownPhase_ = -1;

    std::function<bool(const std::string&)> onSend_;
    std::function<void()> onCancel_, onLeave_, onLeaveNow_, onCallAgain_;
};

#endif
