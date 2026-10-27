#ifndef RAGTIME_SESSION_PAGE_H
#define RAGTIME_SESSION_PAGE_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "ragtime/files/FileTraffic.h"
#include "ragtime/session/SessionChat.h"

class wxBoxSizer;
class wxButton;
class wxListBox;
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
//
// Files (package G) show among the messages as cards: an offer to us with
// Save, Save as..., Decline and its countdown, and "4,096 of 7,000 kept
// from before" when a part is kept; a file moving either way with its
// progress and Cancel; how each ended. Files dropped on the tab, or picked
// with Send file..., queue for the partner; the queue shows above the line
// to type in, to reorder or remove before each starts.
class SessionPage : public wxPanel
{
public:
    SessionPage(wxWindow* parent, const ragtime::session::SessionChat& session,
                const ragtime::files::FileTraffic& files);

    // Send pressed: true when the text went, and the line is cleared.
    void onSend(std::function<bool(const std::string& typed)> fn) { onSend_ = std::move(fn); }
    void onCancel(std::function<void()> fn) { onCancel_ = std::move(fn); }
    void onLeave(std::function<void()> fn) { onLeave_ = std::move(fn); }
    void onLeaveNow(std::function<void()> fn) { onLeaveNow_ = std::move(fn); }
    void onCallAgain(std::function<void()> fn) { onCallAgain_ = std::move(fn); }
    // Re-send on a message's right-click menu, with the message's id.
    void onResend(std::function<void(uint64_t id)> fn) { onResend_ = std::move(fn); }
    // A reaction picked on a message's menu or chip, with its palette code.
    void onReact(std::function<void(uint64_t id, uint8_t code)> fn) { onReact_ = std::move(fn); }
    // Send file... pressed.
    void onSendFile(std::function<void()> fn) { onSendFile_ = std::move(fn); }
    // Inline pictures (docs/STAGE1.md, "Inline pictures"): the picture
    // button, left of the floppy; a picture card's picture as it stands, an
    // <img> linked to href ("" while nothing shows yet), with the stop
    // button linked to stopHref when that isn't empty; and a word that a
    // picture changed, so its card is drawn again.
    void onSendPicture(std::function<void()> fn) { onSendPicture_ = std::move(fn); }
    void pictures(std::function<wxString(uint64_t id, const wxWindow* list, const wxString& href,
                                         const wxString& stopHref)> fn)
    {
        pictureHtml_ = std::move(fn);
    }
    void picturesChanged() { shownFiles_ = ~0ull; }
    // A picture under an earlier session's file line, from the file kept
    // (its path in the session log), and that picture clicked.
    void pastPictures(std::function<wxString(const std::string& path, const wxWindow* list, const wxString& href)> html,
                      std::function<void(const std::string& path)> view)
    {
        pastPictureHtml_ = std::move(html);
        onViewPast_ = std::move(view);
    }
    // Files dropped on the tab.
    void onDropFiles(std::function<void(const std::vector<std::string>& paths)> fn) { onDropFiles_ = std::move(fn); }
    // A card's link or the queue's buttons: "save", "saveas", "decline",
    // "cancel", "open", "folder", "up", "down", and a picture's "view" and
    // "enough" (its stop sign), with the transfer's id.
    void onFileAction(std::function<void(const std::string& action, uint64_t id)> fn)
    {
        onFileAction_ = std::move(fn);
    }

    // Redraws the strip every second, and the messages when they change.
    void refresh(uint64_t nowMs, long utcOffsetS);
    // Pause sending shows a line saying so; with a reason (receive only,
    // outside the US data segments) the line gives it instead.
    void setPaused(bool paused, const wxString& why = wxString());
    void focusCompose();

private:
    friend class SessionMessages;

    // One line of the list: a message, or a file's card.
    struct Item
    {
        bool file = false;
        size_t message = 0;         // the message's index
        uint64_t transfer = 0;      // the file's id
        uint64_t atMs = 0;
    };

    void send();
    void showMenu(int row);
    void rebuild(uint64_t nowMs);
    void refreshQueue();
    wxString cardHtml(const ragtime::files::FileTransfer& t, bool picked, const wxWindow* list) const;

    const ragtime::session::SessionChat& session_;
    const ragtime::files::FileTraffic& files_;
    std::vector<Item> items_;
    std::map<uint64_t, ragtime::files::FileTransfer> transfers_;   // the cards', as of the last rebuild
    std::vector<uint64_t> queued_;                                  // the queue's, top first
    uint64_t nowMs_ = 0;
    long utcOffsetS_ = 0;
    uint64_t shownFiles_ = ~0ull;
    bool liveFiles_ = false;        // a card counts down or moves: redrawn every second
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
    wxButton* sendFile_ = nullptr;
    wxButton* sendPicture_ = nullptr;
    wxPanel* queue_ = nullptr;
    wxStaticText* queueTitle_ = nullptr;
    wxListBox* queueList_ = nullptr;
    uint64_t shownVersion_ = ~0ull;
    std::string shownStrip_;
    bool shownAmber_ = false;
    int shownPhase_ = -1;

    std::function<bool(const std::string&)> onSend_;
    std::function<void()> onCancel_, onLeave_, onLeaveNow_, onCallAgain_, onSendFile_, onSendPicture_;
    std::function<wxString(uint64_t, const wxWindow*, const wxString&, const wxString&)> pictureHtml_;
    std::function<wxString(const std::string&, const wxWindow*, const wxString&)> pastPictureHtml_;
    std::function<void(const std::string&)> onViewPast_;
    std::function<void(const std::vector<std::string>&)> onDropFiles_;
    std::function<void(const std::string&, uint64_t)> onFileAction_;
    std::function<void(uint64_t)> onResend_;
    std::function<void(uint64_t, uint8_t)> onReact_;
};

#endif
