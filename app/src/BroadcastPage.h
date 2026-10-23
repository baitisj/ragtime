#ifndef RAGTIME_BROADCAST_PAGE_H
#define RAGTIME_BROADCAST_PAGE_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <wx/panel.h>

#include "ragtime/broadcast/BroadcastChat.h"

class wxBitmapButton;
class wxBoxSizer;
class wxButton;
class wxStaticText;
class wxTextCtrl;
class MessageList;

// A broadcast file's card among the messages, as the window describes it
// (BroadcastFiles holds the file; the card only shows it).
struct FileCard
{
    std::string what;           // "fred.txt · 7,000 bytes"
    std::string state;          // "Receiving, 12 of 15 pieces", "Saved", "Repairs open until 15:42 (1 h 10 min)"
    bool good = false;          // shown green: Saved, Sent
    bool bad = false;           // shown red: Failed
    // The card's links, in order: (action, label), such as
    // ("receive", "Receive...") and ("ignore", "Ignore").
    std::vector<std::pair<std::string, std::string>> actions;
    // Its right-click menu, the same on the Files tab: a greyed first line
    // when there is one ("Repairs open until 15:42 (1 h 10 min)"), then
    // these, or the links when there are none. An item with no action is
    // greyed: it says why something can't be done.
    std::string title;
    std::vector<std::pair<std::string, std::string>> menu;
};

// The Broadcast tab (docs/STAGE1.md, package E): the messages on D2G, each
// of ours showing Queued, Sending, Sent or Not sent; the line to type in,
// where "@W1AW" at the start addresses one station; the Gear button, a
// speedometer over the gear's short name, which opens the gearshift; and
// Send.
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
    // Broadcast files: the File... button, each card's look, and its links.
    void onBroadcastFile(std::function<void()> fn) { onBroadcastFile_ = std::move(fn); }
    void fileCards(std::function<bool(uint64_t file, FileCard& card)> fn) { fileCard_ = std::move(fn); }
    void onFileAction(std::function<void(uint64_t file, const std::string& action)> fn)
    {
        onFileAction_ = std::move(fn);
    }
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
    // The ham phrases on the line, highlighted as they are typed.
    void showPhrases();
    void showMenu(int row);
    void showFileMenu(const ragtime::broadcast::Message& m);
    void arrange(int width);
    // Redraws the Gear button's speedometer and gear name, in the theme's colours.
    void drawGearButton();

    const ragtime::broadcast::BroadcastChat& chat_;
    MessageList* list_ = nullptr;
    wxStaticText* paused_ = nullptr;
    wxTextCtrl* compose_ = nullptr;
    wxButton* file_ = nullptr;
    wxBitmapButton* gear_ = nullptr;
    wxString gearName_ = "Gear";
    wxButton* send_ = nullptr;
    wxBoxSizer* buttons_ = nullptr;
    wxBoxSizer* row_ = nullptr;
    wxBoxSizer* under_ = nullptr;
    bool stacked_ = false;
    uint64_t shownVersion_ = 0;

    std::function<bool(const std::string&)> onSend_;
    std::function<void()> onGear_;
    std::function<void(uint64_t)> onCancel_, onResend_;
    std::function<void()> onBroadcastFile_;
    std::function<bool(uint64_t, FileCard&)> fileCard_;
    std::function<void(uint64_t, const std::string&)> onFileAction_;
    std::function<std::string(const std::string&)> nameMode_;
};

#endif
