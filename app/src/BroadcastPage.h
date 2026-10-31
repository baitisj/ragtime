#ifndef RAGTIME_BROADCAST_PAGE_H
#define RAGTIME_BROADCAST_PAGE_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <wx/panel.h>

#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Seals.h"

class wxBitmapButton;
class wxBoxSizer;
class wxMenu;
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
    bool picture = false;       // an inline picture, shown under its line
    bool voice = false;         // a voice message, its voice card under its line
    // The card's links, in order: (action, label), such as
    // ("receive", "Receive...") and ("ignore", "Ignore").
    std::vector<std::pair<std::string, std::string>> actions;
    // Its right-click menu, the same on the Files tab: a greyed first line
    // when there is one ("Repairs open until 15:42 (1 h 10 min)"), then
    // these, or the links when there are none. An item with no action is
    // greyed: it says why something can't be done.
    std::string title;
    std::vector<std::pair<std::string, std::string>> menu;
    // A submenu after them when it has items (subLabel, "Repair gear"):
    // radio items (action, label), the one whose action is subChosen ticked.
    std::string subLabel;
    std::vector<std::pair<std::string, std::string>> sub;
    std::string subChosen;
};

// A file card's right-click menu, the same in the chat and on the Files
// tab: its greyed title, its menu (or links), and its submenu, with ids
// from firstId on (FILE_MENU_IDS of them at most). act gets the action
// picked.
constexpr int FILE_MENU_IDS = 48;
void fillFileMenu(wxMenu& menu, const FileCard& card, int firstId, std::function<void(const std::string&)> act);

// The Broadcast tab (docs/STAGE1.md, package E): the messages on D2G; the
// line to type in, where "@W1AW" at the start addresses one station; the
// Gear button, a speedometer over the gear's short name, which opens the
// gearshift; and Send.
//
// A ping is a row of its own, greyed: "Ping W1AW, waiting for its pong",
// then "Pong from W1AW in 12 s: it hears us at −8.0 dB, we hear it at
// −3.5 dB" or "No pong from W1AW in 45 s"; a station's ping to us reads
// "W1AW pinged us; we hear it at −3.5 dB. Pong sent".
//
// A line's reactions show as chips under it (ReactionChips.h); its
// right-click menu has React, and clicking a chip adds ours.
//
// Each message row has a badge on its left (SealBadge.h), and hovering it
// says what it means. Ours show their state (Queued, Sending, Sent,
// Acknowledged, Not sent...) until a seal says more: whether its seal has
// gone, is being repaired, or shows a station holds it. Theirs show only
// whether they match their seal. Seals aren't signed until Stage 2, so a
// badge never claims an identity.
class BroadcastPage : public wxPanel
{
public:
    BroadcastPage(wxWindow* parent, const ragtime::broadcast::BroadcastChat& chat);

    // Send pressed: true when the text went, and the line is cleared.
    void onSend(std::function<bool(const std::string& typed)> fn) { onSend_ = std::move(fn); }
    void onGear(std::function<void()> fn) { onGear_ = std::move(fn); }
    void onCancel(std::function<void(uint64_t id)> fn) { onCancel_ = std::move(fn); }
    void onResend(std::function<void(uint64_t id)> fn) { onResend_ = std::move(fn); }
    // A reaction picked on a line's menu or chip: its palette code, or 0 to
    // take back ours still waiting to go.
    void onReact(std::function<void(uint64_t id, uint8_t code)> fn) { onReact_ = std::move(fn); }
    // A ping row's Ping again (and Ping back, on a station's ping to us):
    // ping goes, and whyNot says why it can't ("in a session"), or "".
    void onPing(std::function<void(const std::string& call)> ping,
                std::function<std::string(const std::string& call)> whyNot)
    {
        onPing_ = std::move(ping);
        whyNoPing_ = std::move(whyNot);
    }
    // Broadcast files: the File... button, each card's look, and its links.
    void onBroadcastFile(std::function<void()> fn) { onBroadcastFile_ = std::move(fn); }
    void fileCards(std::function<bool(uint64_t file, FileCard& card)> fn) { fileCard_ = std::move(fn); }
    void onFileAction(std::function<void(uint64_t file, const std::string& action)> fn)
    {
        onFileAction_ = std::move(fn);
    }
    // Inline pictures (docs/STAGE1.md, "Inline pictures"): the picture
    // button, left of the floppy, and a picture card's picture as it
    // stands, an <img> linked to "view" ("" while nothing shows yet).
    void onBroadcastPicture(std::function<void()> fn) { onBroadcastPicture_ = std::move(fn); }
    void pictures(std::function<wxString(uint64_t file, const wxWindow* list, const wxString& href)> fn)
    {
        pictureHtml_ = std::move(fn);
    }
    // A picture under a file line from the log (from its path), and that
    // picture clicked.
    void pastPictures(std::function<wxString(const std::string& path, const wxWindow* list, const wxString& href)> html,
                      std::function<void(const std::string& path)> view)
    {
        pastPictureHtml_ = std::move(html);
        onViewPast_ = std::move(view);
    }
    // Voice cards (docs/STAGE1.md, "Voice cards"): a card's button and
    // strip by its key ("b<id>" a broadcast file, "p<path>" a file line
    // from the log; "" while nothing shows), and a card's link
    // ("voice:play:0", "voice:stop:0", "voice:seek:<frame>") clicked.
    void voices(std::function<wxString(const std::string& key, const wxWindow* list, bool picked)> html,
                std::function<void(const std::string& key, const std::string& href)> act)
    {
        voiceHtml_ = std::move(html);
        onVoice_ = std::move(act);
    }
    // Redraws the cards in view (a playhead moving), without a rebuild.
    void refreshVoices();
    // Each message's seal badge, and a number that moves when any changes.
    using SealMark = ragtime::broadcast::Seals::ItemMark;
    void sealMarks(std::function<SealMark(const ragtime::broadcast::Message&)> fn, std::function<uint64_t()> version)
    {
        sealMark_ = std::move(fn);
        sealsVersion_ = std::move(version);
    }
    // Turns a Data2G mode name into the short name shown to people ("E2C").
    void nameModes(std::function<std::string(const std::string& mode)> fn) { nameMode_ = std::move(fn); }

    // Redraws the messages when the chat has changed.
    void refresh();
    void setGearLabel(const wxString& label, const wxString& tooltip, double bitsPerSecond = 0.0);
    // Pause sending shows a line saying so; with a reason (receive only,
    // outside the US data segments) the line gives it instead.
    void setPaused(bool paused, const wxString& why = wxString());
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
    void showPingMenu(const ragtime::broadcast::Message& m);
    void arrange(int width);
    // Redraws the Gear button's speedometer and gear name, in the theme's colours.
    void drawGearButton();

    const ragtime::broadcast::BroadcastChat& chat_;
    MessageList* list_ = nullptr;
    wxStaticText* paused_ = nullptr;
    wxTextCtrl* compose_ = nullptr;
    wxButton* file_ = nullptr;
    wxButton* picture_ = nullptr;
    wxBitmapButton* gear_ = nullptr;
    wxString gearName_ = "Gear";
    double gearSpeed_ = 0.0;  // bit/s, for the needle; 0 rests it at the left
    wxButton* send_ = nullptr;
    wxBoxSizer* buttons_ = nullptr;
    wxBoxSizer* row_ = nullptr;
    wxBoxSizer* under_ = nullptr;
    bool stacked_ = false;
    uint64_t shownVersion_ = 0;
    uint64_t shownSeals_ = 0;

    std::function<bool(const std::string&)> onSend_;
    std::function<void()> onGear_;
    std::function<void(uint64_t)> onCancel_, onResend_;
    std::function<void(uint64_t, uint8_t)> onReact_;
    std::function<void(const std::string&)> onPing_;
    std::function<std::string(const std::string&)> whyNoPing_;
    std::function<void()> onBroadcastFile_, onBroadcastPicture_;
    std::function<wxString(uint64_t, const wxWindow*, const wxString&)> pictureHtml_;
    std::function<wxString(const std::string&, const wxWindow*, const wxString&)> pastPictureHtml_;
    std::function<void(const std::string&)> onViewPast_;
    std::function<wxString(const std::string&, const wxWindow*, bool)> voiceHtml_;
    std::function<void(const std::string&, const std::string&)> onVoice_;
    std::function<bool(uint64_t, FileCard&)> fileCard_;
    std::function<void(uint64_t, const std::string&)> onFileAction_;
    std::function<std::string(const std::string&)> nameMode_;
    std::function<SealMark(const ragtime::broadcast::Message&)> sealMark_;
    std::function<uint64_t()> sealsVersion_;
};

#endif
