#ifndef RAGTIME_BROADCAST_PAGE_H
#define RAGTIME_BROADCAST_PAGE_H

#include <cstdint>
#include <functional>
#include <string>

#include <wx/panel.h>

#include "ragtime/broadcast/BroadcastChat.h"

class wxBoxSizer;
class wxButton;
class wxStaticText;
class wxTextCtrl;
class MessageList;

// The Broadcast tab (docs/STAGE1.md, package E): the messages on D2G, each
// of ours showing Queued, Sending, Sent or Not sent; the line to type in,
// where "@W1AW" at the start addresses one station; the Gear button, which
// shows the gear and opens the gearshift; and Send.
//
// Each message row leaves room on its left for the seal badge. Signing
// comes in Stage 2, so the badge stays hidden until then rather than
// marking every message Unsigned.
class BroadcastPage : public wxPanel
{
public:
    BroadcastPage(wxWindow* parent, const ragtime::broadcast::BroadcastChat& chat);

    // Send pressed: true when the text went, and the line is cleared.
    void onSend(std::function<bool(const std::string& typed)> fn) { onSend_ = std::move(fn); }
    void onGear(std::function<void()> fn) { onGear_ = std::move(fn); }
    void onCancel(std::function<void(uint64_t id)> fn) { onCancel_ = std::move(fn); }
    void onResend(std::function<void(uint64_t id)> fn) { onResend_ = std::move(fn); }
    // Turns a Data2G mode name into the short name shown to people ("E2C").
    void nameModes(std::function<std::string(const std::string& mode)> fn) { nameMode_ = std::move(fn); }

    // Redraws the messages when the chat has changed.
    void refresh();
    void setGearLabel(const wxString& label, const wxString& tooltip);
    void setPaused(bool paused);
    // "@W1AW " at the start of the line, and the cursor after it.
    void address(const std::string& call);
    void focusCompose();

private:
    friend class MessageList;

    void send();
    void showMenu(int row);
    void arrange(int width);

    const ragtime::broadcast::BroadcastChat& chat_;
    MessageList* list_ = nullptr;
    wxStaticText* paused_ = nullptr;
    wxTextCtrl* compose_ = nullptr;
    wxButton* gear_ = nullptr;
    wxButton* send_ = nullptr;
    wxBoxSizer* buttons_ = nullptr;
    wxBoxSizer* row_ = nullptr;
    wxBoxSizer* under_ = nullptr;
    bool stacked_ = false;
    uint64_t shownVersion_ = 0;

    std::function<bool(const std::string&)> onSend_;
    std::function<void()> onGear_;
    std::function<void(uint64_t)> onCancel_, onResend_;
    std::function<std::string(const std::string&)> nameMode_;
};

#endif
