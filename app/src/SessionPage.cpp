#include "SessionPage.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/datetime.h>
#include <wx/dnd.h>
#include <wx/htmllbox.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "DrawnIcon.h"
#include "Html.h"
#include "PhraseHighlight.h"

using namespace ragtime::session;
using ragtime::files::FileRow;
using ragtime::files::FileTransfer;
using State = Message::State;
using Phase = SessionChat::Phase;
using FState = FileTransfer::State;

namespace
{

const wxColour DELIVERED(0x1e, 0x6b, 0x34);    // green, as the Sealed badge
const wxColour NOT_DELIVERED(0xb4, 0x23, 0x18); // red, as the Mismatch badge
const wxColour QUIET(0x8a, 0x4b, 0x00);         // amber, as the Repair Asked badge

// Room for the seal badge (16 px) in every row, as in the Broadcast tab.
constexpr int BADGE_ROOM = 20;

const int ID_COPY = wxID_HIGHEST + 501;
const int ID_RESEND = wxID_HIGHEST + 502;

// "14:32" today, "22 Oct 14:32" on another day (an earlier session's).
wxString clock(uint64_t ms)
{
    wxDateTime at((time_t)(ms / 1000));
    return at.Format(at.IsSameDate(wxDateTime::Today()) ? "%H:%M" : "%d %b %H:%M");
}

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

wxString link(const char* action, uint64_t id, const wxString& label)
{
    return wxString::Format("<a href=\"%s:%llu\">", action, (unsigned long long)id) + label + "</a>";
}

const char* const DOT = " \xc2\xb7 ";

// Files dropped anywhere on the tab.
class FileDrop : public wxFileDropTarget
{
public:
    explicit FileDrop(std::function<void(const std::vector<std::string>&)>* fn) : fn_(fn) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& names) override
    {
        if (fn_ == nullptr || !*fn_) return false;
        std::vector<std::string> paths;
        for (const wxString& name : names) paths.push_back(std::string(name.utf8_str()));
        (*fn_)(paths);
        return true;
    }

private:
    std::function<void(const std::vector<std::string>&)>* fn_;
};

} // namespace

class SessionMessages : public wxHtmlListBox
{
public:
    explicit SessionMessages(SessionPage* owner)
        : wxHtmlListBox(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME)
        , owner_(owner)
    {
        SetMargins(FromDIP(4), FromDIP(1));
    }

    const Message* at(size_t n) const
    {
        if (n >= owner_->items_.size() || owner_->items_[n].file) return nullptr;
        const auto& all = owner_->session_.messages();
        size_t index = owner_->items_[n].message;
        return index < all.size() ? &all[index] : nullptr;
    }

    const FileTransfer* fileAt(size_t n) const
    {
        if (n >= owner_->items_.size() || !owner_->items_[n].file) return nullptr;
        auto it = owner_->transfers_.find(owner_->items_[n].transfer);
        return it == owner_->transfers_.end() ? nullptr : &it->second;
    }

protected:
    void OnLinkClicked(size_t, const wxHtmlLinkInfo& info) override
    {
        wxString href = info.GetHref();
        wxString action = href.BeforeFirst(':');
        unsigned long long id = 0;
        if (!href.AfterFirst(':').ToULongLong(&id) || !owner_->onFileAction_) return;
        // After this returns: the action may open a dialog or change the list.
        std::string what(action.utf8_str());
        SessionPage* owner = owner_;
        owner_->CallAfter([owner, what, id]() { owner->onFileAction_(what, (uint64_t)id); });
    }

    wxString OnGetItem(size_t n) const override
    {
        if (const FileTransfer* t = fileAt(n)) return owner_->cardHtml(*t, IsSelected(n));
        const Message* m = at(n);
        if (m == nullptr) return wxString();
        bool picked = IsSelected(n);
        // A session that has ended reads in grey, and so do the earlier
        // ones above it.
        bool live = owner_->session_.active() && !m->past;
        auto colour = [picked](const wxColour& c) {
            return picked ? wxString() : " color=\"" + htmlColour(c) + "\"";
        };
        wxString body = live ? wxString() : colour(secondaryText());

        if (m->kind == Message::Kind::Rule)
        {
            // Where an earlier session ended: the log carries on below.
            std::string caption = "Session with " + owner_->session_.peer() + " ended "
                                  + std::string(clock(m->atMs).utf8_str());
            if (m->beganMs != 0 && m->atMs > m->beganMs) caption += DOT + sessionLength(m->atMs - m->beganMs);
            wxString html;
            html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
                 << "<td width=\"50%\" valign=\"middle\"><hr></td>"
                 << "<td nowrap valign=\"middle\"><font size=\"-1\"" << colour(secondaryText()) << ">"
                 << htmlEscape(caption) << "</font></td>"
                 << "<td width=\"50%\" valign=\"middle\"><hr></td>"
                 << "</tr></table>";
            return html;
        }
        wxString what = htmlEscape(m->text);
        if (m->kind == Message::Kind::File) what = "<i>File</i>&nbsp; " + what;

        wxString state;
        if (m->ours)
        {
            wxColour c = secondaryText();
            if (m->state == State::Delivered) c = DELIVERED;
            if (m->state == State::NotDelivered) c = NOT_DELIVERED;
            state << "<font" << colour(c) << ">" << stateLabel(m->state) << "</font>";
        }

        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"" << BADGE_ROOM << "\"></td>"
             << "<td valign=\"top\" nowrap><font" << colour(secondaryText()) << ">" << clock(m->atMs)
             << "</font></td>"
             << "<td valign=\"top\" width=\"100%\"><font" << body << "><b>" << htmlEscape(m->from) << "</b>&nbsp; "
             << what << "</font></td>"
             << "<td valign=\"top\" nowrap align=\"right\">" << state << "</td>"
             << "</tr></table>";
        return html;
    }

private:
    SessionPage* owner_;
};

SessionPage::SessionPage(wxWindow* parent, const SessionChat& session, const ragtime::files::FileTraffic& files)
    : wxPanel(parent)
    , session_(session)
    , files_(files)
{
    // The status strip.
    strip_ = new wxPanel(this);
    stripText_ = new wxStaticText(strip_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    stripText_->SetFont(stripText_->GetFont().Bold());
    cancel_ = new wxButton(strip_, wxID_ANY, "Cancel");
    cancel_->SetToolTip("Stops the call (ABORT)");
    leave_ = new wxButton(strip_, wxID_ANY, "Leave");
    leave_->SetToolTip("Leaves the session once what you sent is acknowledged (DISCONNECT)");
    leaveNow_ = new wxButton(strip_, wxID_ANY, "Leave now");
    leaveNow_->SetToolTip("Leaves at once, without waiting for acknowledgements (ABORT)");
    callAgain_ = new wxButton(strip_, wxID_ANY, "Call again");
    auto* stripSizer = new wxBoxSizer(wxHORIZONTAL);
    stripSizer->Add(stripText_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    for (wxButton* b : {cancel_, leave_, leaveNow_, callAgain_})
        stripSizer->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(4));
    strip_->SetSizer(stripSizer);

    list_ = new SessionMessages(this);
    paused_ = new wxStaticText(this, wxID_ANY,
                               "Sending is paused: messages wait as Queued until you press Pause sending again.");
    paused_->SetForegroundColour(secondaryText());
    paused_->Hide();
    compose_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | PHRASE_ENTRY_STYLE);
    compose_->SetMaxLength((unsigned long)SessionChat::MAX_TYPED);
    send_ = new wxButton(this, wxID_ANY, "Send");
    // A floppy disk rather than "Send file...", as on the Broadcast tab.
    sendFile_ = new wxBitmapButton(this, wxID_ANY, floppyIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    sendFile_->SetToolTip("Send a file: offers files to the station you're in a session with; you can also drop "
                          "files on this tab. They go one at a time, in the order queued.");
    sendFile_->SetMinSize(wxSize(-1, std::max(sendFile_->GetBestSize().y, send_->GetBestSize().y)));
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
        sendFile_->SetBitmap(floppyIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
        e.Skip();
    });
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(compose_, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(send_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    row->Add(sendFile_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    // The send queue: files waiting their turn, to reorder or remove.
    queue_ = new wxPanel(this);
    queueTitle_ = new wxStaticText(queue_, wxID_ANY, "Waiting their turn");
    queueTitle_->SetForegroundColour(secondaryText());
    queueList_ = new wxListBox(queue_, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(64)));
    auto* up = new wxButton(queue_, wxID_UP, "Up", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* down = new wxButton(queue_, wxID_DOWN, "Down", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* remove = new wxButton(queue_, wxID_REMOVE, "Remove", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* queueButtons = new wxBoxSizer(wxVERTICAL);
    for (wxButton* b : {up, down, remove}) queueButtons->Add(b, 0, wxEXPAND | wxBOTTOM, 2);
    auto* queueRow = new wxBoxSizer(wxHORIZONTAL);
    queueRow->Add(queueList_, 1, wxEXPAND);
    queueRow->Add(queueButtons, 0, wxLEFT, 6);
    auto* queueSizer = new wxBoxSizer(wxVERTICAL);
    queueSizer->Add(queueTitle_, 0, wxBOTTOM, 2);
    queueSizer->Add(queueRow, 1, wxEXPAND);
    queue_->SetSizer(queueSizer);
    queue_->Hide();
    auto queueAction = [this](const char* action) {
        int n = queueList_->GetSelection();
        if (n == wxNOT_FOUND || (size_t)n >= queued_.size() || !onFileAction_) return;
        onFileAction_(action, queued_[(size_t)n]);
    };
    up->Bind(wxEVT_BUTTON, [queueAction](wxCommandEvent&) { queueAction("up"); });
    down->Bind(wxEVT_BUTTON, [queueAction](wxCommandEvent&) { queueAction("down"); });
    remove->Bind(wxEVT_BUTTON, [queueAction](wxCommandEvent&) { queueAction("cancel"); });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(strip_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(queue_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(paused_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->AddSpacer(6);
    SetSizer(sizer);

    compose_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { send(); });
    compose_->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        std::string typed(compose_->GetValue().utf8_str());
        highlightPhrases(compose_, typed, ragtime::text::HamText::phraseSpans(typed));
        e.Skip();
    });
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
    sendFile_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onSendFile_) onSendFile_();
    });
    // Drop targets are each window's own.
    SetDropTarget(new FileDrop(&onDropFiles_));
    list_->SetDropTarget(new FileDrop(&onDropFiles_));
    compose_->SetDropTarget(new FileDrop(&onDropFiles_));
    cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onCancel_) onCancel_();
    });
    leave_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onLeave_) onLeave_();
    });
    leaveNow_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onLeaveNow_) onLeaveNow_();
    });
    callAgain_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onCallAgain_) onCallAgain_();
    });
    list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxPoint at = list_->ScreenToClient(e.GetPosition());
        int row = e.GetPosition() == wxDefaultPosition ? list_->GetSelection() : list_->VirtualHitTest(at.y);
        if (row != wxNOT_FOUND) list_->SetSelection(row);
        showMenu(row);
    });
    list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        int row = list_->VirtualHitTest(e.GetPosition().y);
        const Message* m = row == wxNOT_FOUND ? nullptr : list_->at((size_t)row);
        const FileTransfer* t = row == wxNOT_FOUND ? nullptr : list_->fileAt((size_t)row);
        wxString tip;
        if (m != nullptr && m->kind == Message::Kind::Text) tip = wxDateTime((time_t)m->time).FormatISOCombined(' ') + wxString::Format(", item %u", m->seq);
        if (t != nullptr && !t->path.empty()) tip = utf8(t->path);
        list_->SetToolTip(tip);
        e.Skip();
    });
    refresh(0, 0);
}

void SessionPage::refresh(uint64_t nowMs, long utcOffsetS)
{
    SessionChat::Strip strip = session_.strip(nowMs, utcOffsetS);
    int phase = (int)session_.phase();
    if (strip.text != shownStrip_ || strip.amber != shownAmber_ || phase != shownPhase_)
    {
        shownStrip_ = strip.text;
        shownAmber_ = strip.amber;
        stripText_->SetLabel(wxString::FromUTF8(strip.text));
        stripText_->SetToolTip(wxString::FromUTF8(strip.text));
        // Amber when the link is quiet; a session that's up stands out from
        // the greyed one that has ended.
        wxColour surface = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
        if (strip.amber)
            strip_->SetBackgroundColour(blendColour(surface, wxColour(0xf7, 0xc6, 0x00), 0.30));
        else if (strip.live)
            strip_->SetBackgroundColour(blendColour(surface, ACCENT_BLUE, 0.10));
        else
            strip_->SetBackgroundColour(wxNullColour);
        stripText_->SetForegroundColour(strip.amber ? QUIET : strip.live ? wxNullColour : secondaryText());

        Phase p = session_.phase();
        bool calling = p == Phase::Calling || (p == Phase::Leaving && session_.connectedMs() == 0);
        cancel_->Show(p == Phase::Calling);
        leave_->Show(p == Phase::Connected);
        leaveNow_->Show(p == Phase::Leaving && !calling);
        callAgain_->Show(p == Phase::Ended && !session_.peer().empty());
        callAgain_->SetLabel("Call " + wxString::FromUTF8(session_.peer()) + " again");
        bool connected = p == Phase::Connected;
        compose_->Enable(connected);
        send_->Enable(connected);
        sendFile_->Enable(connected);
        compose_->SetHint(connected ? "Message to " + wxString::FromUTF8(session_.peer()) + " in the session"
                                    : wxString("No session: nothing to type into"));
        if (phase != shownPhase_ && connected && IsShownOnScreen()) CallAfter([this]() { compose_->SetFocus(); });
        shownPhase_ = phase;
        strip_->Layout();
        strip_->Refresh();
        Layout();
    }

    nowMs_ = nowMs;
    utcOffsetS_ = utcOffsetS;
    if (shownVersion_ != session_.version() || shownFiles_ != files_.changes() || liveFiles_)
    {
        shownVersion_ = session_.version();
        shownFiles_ = files_.changes();
        size_t before = items_.size();
        bool atEnd = list_->GetItemCount() == 0 || list_->GetVisibleRowsEnd() >= list_->GetItemCount();
        rebuild(nowMs);
        size_t count = items_.size();
        list_->SetItemCount(count);
        list_->RefreshAll();
        if (atEnd && count > 0 && count != before) list_->ScrollToRow(count - 1);
        refreshQueue();
    }
}

void SessionPage::rebuild(uint64_t nowMs)
{
    items_.clear();
    transfers_.clear();
    liveFiles_ = false;
    const auto& messages = session_.messages();
    for (size_t i = 0; i < messages.size(); i++)
    {
        Item item;
        item.message = i;
        item.atMs = messages[i].atMs;
        items_.push_back(item);
    }

    // The session's files: those offered since it began, and any still
    // going with the partner. One waiting its turn is in the queue instead.
    const std::string& peer = session_.peer();
    uint64_t start = session_.connectedMs() != 0 ? session_.connectedMs() : session_.sinceMs();
    std::vector<Item> cards;
    if (!peer.empty())
    {
        for (const FileTransfer& t : files_.files().transfers(nowMs))
        {
            if (t.peer != peer || t.state == FState::Waiting) continue;
            if (!t.live() && t.atMs < start) continue;
            Item item;
            item.file = true;
            item.transfer = t.id;
            item.atMs = std::max(t.atMs, start);
            cards.push_back(item);
            transfers_[t.id] = t;
            liveFiles_ = liveFiles_ || t.live();
        }
    }
    std::stable_sort(cards.begin(), cards.end(), [](const Item& a, const Item& b) { return a.atMs < b.atMs; });
    std::vector<Item> merged;
    std::merge(items_.begin(), items_.end(), cards.begin(), cards.end(), std::back_inserter(merged),
               [](const Item& a, const Item& b) { return a.atMs < b.atMs; });
    items_.swap(merged);
}

void SessionPage::refreshQueue()
{
    std::vector<uint64_t> queued;
    wxArrayString names;
    const std::string& peer = session_.peer();
    for (const FileTransfer& t : files_.files().transfers())
    {
        if (!t.outgoing || t.peer != peer || t.state != FState::Waiting) continue;
        queued.push_back(t.id);
        names.Add(utf8(t.name + " (" + ragtime::files::sizeText(t.size) + ")"));
    }
    bool show = !queued.empty() && !peer.empty();
    if (queued != queued_)
    {
        uint64_t picked = 0;
        int n = queueList_->GetSelection();
        if (n != wxNOT_FOUND && (size_t)n < queued_.size()) picked = queued_[(size_t)n];
        queued_ = queued;
        queueList_->Set(names);
        for (size_t i = 0; i < queued_.size(); i++)
        {
            if (queued_[i] == picked) queueList_->SetSelection((int)i);
        }
        queueTitle_->SetLabel(session_.phase() == Phase::Connected
                                  ? wxString::Format("Waiting their turn for %s (%zu)", utf8(peer), queued_.size())
                                  : wxString::Format("Queued for the next session with %s (%zu)", utf8(peer),
                                                     queued_.size()));
    }
    if (queue_->IsShown() != show)
    {
        queue_->Show(show);
        Layout();
    }
}

wxString SessionPage::cardHtml(const FileTransfer& t, bool picked) const
{
    FileRow row = files_.row(t, nowMs_);
    std::string size = ragtime::files::sizeText(t.size);
    std::string progress = ragtime::files::progressText(t.handed > t.done && !t.outgoing ? t.handed : t.done, t.size);
    std::string rate = ragtime::files::rateText(row.bytesPerSecond);
    wxString name = "<b>" + htmlEscape(t.name) + "</b>";
    wxString who = "<b>" + htmlEscape(t.peer) + "</b>";
    auto grey = [picked](const wxString& text) {
        return picked ? text : "<font color=\"" + htmlColour(secondaryText()) + "\">" + text + "</font>";
    };
    auto tinted = [picked](const wxString& text, const wxColour& c) {
        return picked ? text : "<font color=\"" + htmlColour(c) + "\">" + text + "</font>";
    };
    std::string moving = ragtime::files::stateText(row, nowMs_, utcOffsetS_);
    if (!rate.empty()) moving += DOT + rate;

    wxString body;
    if (!t.outgoing)
    {
        switch (t.state)
        {
            case FState::Asking:
                body << who << " offers " << name << ", " << htmlEscape(size);
                if (t.expiresInMs != 0)
                    body << grey(utf8(DOT + std::string("answer within ") + ragtime::session::clockLength(t.expiresInMs)));
                if (t.kept > 0)
                    body << "<br>" << htmlEscape(ragtime::files::progressText(t.kept, t.size))
                         << " kept from before: Save carries on from there.";
                if (!t.error.empty()) body << "<br>" << grey(htmlEscape(t.error));
                body << "<br>" << link("save", t.id, "Save") << "&nbsp;&nbsp; " << link("saveas", t.id, "Save as...")
                     << "&nbsp;&nbsp; " << link("decline", t.id, "Decline");
                break;
            case FState::Receiving:
                body << "Receiving " << name << " from " << who << grey(utf8(DOT + progress + DOT + moving));
                if (t.autoAccepted) body << grey(" (taken without asking)");
                if (t.resumedFrom > 0)
                    body << "<br>" << grey("Carrying on from " + htmlEscape(ragtime::files::groupedNumber(t.resumedFrom))
                                           + " bytes kept from before");
                body << "&nbsp;&nbsp; " << link("cancel", t.id, "Cancel");
                break;
            case FState::Saved:
                body << name << " from " << who << ": " << tinted("saved", DELIVERED) << " as "
                     << htmlEscape(t.path) << "&nbsp;&nbsp; " << link("open", t.id, "Open") << "&nbsp;&nbsp; "
                     << link("folder", t.id, "Open folder");
                break;
            default:
            {
                ragtime::files::FinishedFile f;
                f.outgoing = false;
                f.peer = t.peer;
                f.reason = t.error;
                f.result = t.state == FState::Declined         ? ragtime::files::FinishedFile::Result::Declined
                           : t.state == FState::Cancelled      ? ragtime::files::FinishedFile::Result::Cancelled
                           : t.state == FState::CancelledThere ? ragtime::files::FinishedFile::Result::CancelledThere
                           : t.state == FState::Expired        ? ragtime::files::FinishedFile::Result::Expired
                                                               : ragtime::files::FinishedFile::Result::Failed;
                body << name << " from " << who << ": " << tinted(htmlEscape(ragtime::files::resultLabel(f)),
                                                                  NOT_DELIVERED);
                if (t.done > 0 && t.done < t.size && t.state != FState::Declined)
                    body << grey(utf8(DOT + ragtime::files::progressText(t.done, t.size) + " kept, to carry on next time"));
                break;
            }
        }
    }
    else
    {
        switch (t.state)
        {
            case FState::Offered:
                body << "You offered " << name << ", " << htmlEscape(size) << grey(utf8(DOT + std::string("waiting for an answer")));
                if (t.expiresInMs != 0) body << grey(utf8(DOT + ragtime::session::clockLength(t.expiresInMs)));
                body << "&nbsp;&nbsp; " << link("cancel", t.id, "Cancel");
                break;
            case FState::Sending:
                body << "Sending " << name << grey(utf8(DOT + progress + DOT + moving));
                if (t.resumedFrom > 0)
                    body << "<br>" << grey(who + " had " + htmlEscape(ragtime::files::groupedNumber(t.resumedFrom))
                                           + " bytes from before; the rest is going");
                body << "&nbsp;&nbsp; " << link("cancel", t.id, "Cancel");
                break;
            case FState::Delivered:
                body << name << ": " << tinted(htmlEscape(t.peer + " saved it"), DELIVERED);
                break;
            default:
            {
                ragtime::files::FinishedFile f;
                f.outgoing = true;
                f.peer = t.peer;
                f.reason = t.error;
                f.result = t.state == FState::Declined         ? ragtime::files::FinishedFile::Result::Declined
                           : t.state == FState::Cancelled      ? ragtime::files::FinishedFile::Result::Cancelled
                           : t.state == FState::CancelledThere ? ragtime::files::FinishedFile::Result::CancelledThere
                           : t.state == FState::Expired        ? ragtime::files::FinishedFile::Result::Expired
                           : t.state == FState::NotTaken       ? ragtime::files::FinishedFile::Result::NotTaken
                                                               : ragtime::files::FinishedFile::Result::Failed;
                body << name << ": " << tinted(htmlEscape(ragtime::files::resultLabel(f)), NOT_DELIVERED);
                break;
            }
        }
    }

    wxColour surface = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
    wxString card = picked ? wxString() : " bgcolor=\"" + htmlColour(blendColour(surface, ACCENT_BLUE, 0.07)) + "\"";
    wxString html;
    html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
         << "<td width=\"" << BADGE_ROOM << "\"></td>"
         << "<td valign=\"top\" nowrap>" << grey(clock(t.atMs)) << "</td>"
         << "<td valign=\"top\" width=\"100%\"" << card << ">" << body << "</td>"
         << "</tr></table>";
    return html;
}

void SessionPage::setPaused(bool paused)
{
    if (paused_->IsShown() == paused) return;
    paused_->Show(paused);
    Layout();
}

void SessionPage::focusCompose()
{
    if (compose_->IsEnabled()) compose_->SetFocus();
}

void SessionPage::send()
{
    std::string typed(compose_->GetValue().utf8_str());
    if (onSend_ && onSend_(typed)) compose_->Clear();
    compose_->SetFocus();
}

void SessionPage::showMenu(int row)
{
    const Message* m = row == wxNOT_FOUND ? nullptr : list_->at((size_t)row);
    if (m == nullptr || m->kind == Message::Kind::Rule) return;
    uint64_t id = m->id;
    wxString text = wxString::FromUTF8(m->text);
    wxMenu menu;
    if (SessionChat::canResend(*m))
    {
        // Into the session that is up; greyed, saying why, when none is.
        bool connected = session_.phase() == SessionChat::Phase::Connected;
        menu.Append(ID_RESEND, connected ? wxString("Re-send")
                                         : wxString("Re-send (no session up)"))->Enable(connected);
        menu.AppendSeparator();
    }
    menu.Append(ID_COPY, "Copy text");
    menu.Bind(wxEVT_MENU, [=](wxCommandEvent& e) {
        if (e.GetId() == ID_RESEND)
        {
            if (onResend_) onResend_(id);
        }
        else if (wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&menu);
}
