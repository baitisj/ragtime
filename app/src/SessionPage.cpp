#include "SessionPage.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/datetime.h>
#include <wx/htmllbox.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "Html.h"

using namespace ragtime::session;
using State = Message::State;
using Phase = SessionChat::Phase;

namespace
{

const wxColour DELIVERED(0x1e, 0x6b, 0x34);    // green, as the Sealed badge
const wxColour NOT_DELIVERED(0xb4, 0x23, 0x18); // red, as the Mismatch badge
const wxColour QUIET(0x8a, 0x4b, 0x00);         // amber, as the Repair Asked badge

// Room for the seal badge (16 px) in every row, as in the Broadcast tab.
constexpr int BADGE_ROOM = 20;

const int ID_COPY = wxID_HIGHEST + 501;

wxString clock(uint64_t ms)
{
    return wxDateTime((time_t)(ms / 1000)).Format("%H:%M");
}

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
        const auto& all = owner_->session_.messages();
        return n < all.size() ? &all[n] : nullptr;
    }

protected:
    wxString OnGetItem(size_t n) const override
    {
        const Message* m = at(n);
        if (m == nullptr) return wxString();
        bool picked = IsSelected(n);
        // A session that has ended reads in grey.
        bool live = owner_->session_.active();
        auto colour = [picked](const wxColour& c) {
            return picked ? wxString() : " color=\"" + htmlColour(c) + "\"";
        };
        wxString body = live ? wxString() : colour(secondaryText());

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
             << htmlEscape(m->text) << "</font></td>"
             << "<td valign=\"top\" nowrap align=\"right\">" << state << "</td>"
             << "</tr></table>";
        return html;
    }

private:
    SessionPage* owner_;
};

SessionPage::SessionPage(wxWindow* parent, const SessionChat& session)
    : wxPanel(parent)
    , session_(session)
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
    compose_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    compose_->SetMaxLength((unsigned long)SessionChat::MAX_TYPED);
    send_ = new wxButton(this, wxID_ANY, "Send");
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(compose_, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(send_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(strip_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(paused_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->AddSpacer(6);
    SetSizer(sizer);

    compose_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { send(); });
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
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
        wxString tip;
        if (m != nullptr) tip = wxDateTime((time_t)m->time).FormatISOCombined(' ') + wxString::Format(", item %u", m->seq);
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
        compose_->SetHint(connected ? "Message to " + wxString::FromUTF8(session_.peer()) + " in the session"
                                    : wxString("No session: nothing to type into"));
        if (phase != shownPhase_ && connected && IsShownOnScreen()) CallAfter([this]() { compose_->SetFocus(); });
        shownPhase_ = phase;
        strip_->Layout();
        strip_->Refresh();
        Layout();
    }

    if (shownVersion_ != session_.version() || list_->GetItemCount() != session_.messages().size())
    {
        shownVersion_ = session_.version();
        size_t count = session_.messages().size();
        bool atEnd = list_->GetItemCount() == 0 || list_->GetVisibleRowsEnd() >= list_->GetItemCount();
        list_->SetItemCount(count);
        list_->RefreshAll();
        if (atEnd && count > 0) list_->ScrollToRow(count - 1);
    }
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
    if (m == nullptr) return;
    wxString text = wxString::FromUTF8(m->text);
    wxMenu menu;
    menu.Append(ID_COPY, "Copy text");
    menu.Bind(wxEVT_MENU, [=](wxCommandEvent&) {
        if (wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&menu);
}
