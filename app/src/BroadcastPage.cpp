#include "BroadcastPage.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/tglbtn.h>
#include <wx/clipbrd.h>
#include <wx/datetime.h>
#include <wx/htmllbox.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#if defined(__WXGTK3__)
#include <gtk/gtk.h>
#endif

#include "ChatList.h"
#include "DrawnIcon.h"
#include "GearshiftIcon.h"
#include "Html.h"
#include "PhraseHighlight.h"
#include "ReactionChips.h"
#include "SealBadge.h"
#include "Theme.h"
#include "ragtime/gearbox/Gearbox.h"

using namespace ragtime::broadcast;
using State = Message::State;

namespace
{

const char* const PAUSED_LINE = "Sending is paused: messages wait as Queued until you press Pause sending again.";


// Room for the seal badge (SEAL_BADGE_PX) in every row.
constexpr int BADGE_ROOM = SEAL_BADGE_PX + 4;

// "14:32" today, "22 Oct 14:32" on another day (from the log).
wxString clock(uint64_t ms);

std::string joinCalls(const std::vector<std::string>& calls)
{
    std::string out;
    for (size_t i = 0; i < calls.size(); i++) out += (i == 0 ? "" : i + 1 == calls.size() ? " and " : ", ") + calls[i];
    return out;
}

// An @message of ours whose station sent its Ack.
bool acknowledged(const Message& m)
{
    return m.ours && !m.past && m.state == State::Acknowledged;
}

// An @message of ours that went, its Ack not back yet.
bool awaitingAck(const Message& m)
{
    return m.ours && !m.past && m.state == State::Sent && m.ackWanted;
}

// The badge on a message's left (Jeff, 2026-10-24): ours shows its state
// (StatusBadge) until a seal says more. Once its seal has gone the seal's
// badge shows (Held's for an acknowledged @message), Repairing while it
// goes again, and Awaiting while a sealed line's seal hasn't gone (an
// @message keeps its tick, as the Ack matters more). Theirs shows only the
// seal's. What came before Ragtime last closed shows its state alone.
wxString badgeFor(const Message& m, ragtime::broadcast::Seals::Mark mark, const wxWindow* window)
{
    using Mark = ragtime::broadcast::Seals::Mark;
    if (!m.ours) return m.past ? wxString() : sealBadgeHtml(mark, window);
    switch (m.state)
    {
        case State::Queued:
            return statusBadgeHtml(StatusBadge::Queued, window);
        case State::Sending:
            return statusBadgeHtml(StatusBadge::Sending, window);
        case State::Held:
            return statusBadgeHtml(StatusBadge::Held, window);
        case State::NotSent:
            return statusBadgeHtml(StatusBadge::NotSent, window);
        case State::Cancelled:
            return statusBadgeHtml(StatusBadge::Cancelled, window);
        case State::Sent:
        case State::Acknowledged:
        case State::Heard:
            break;
    }
    bool acked = m.state == State::Acknowledged;
    if (m.past || mark == Mark::None || (mark == Mark::Awaiting && (m.ackWanted || acked)))
        return statusBadgeHtml(acked ? StatusBadge::Acknowledged : StatusBadge::Sent, window);
    if (acked && mark == Mark::Sealed) return sealBadgeHtml(Mark::Held, window);
    return sealBadgeHtml(mark, window);
}

// What a message's seal badge means, for the greyed line atop its menu;
// empty when there is nothing to say.
std::string sealLine(const Message& m, const ragtime::broadcast::Seals::ItemMark& mark)
{
    using Mark = ragtime::broadcast::Seals::Mark;
    std::string at = mark.sealedMs == 0 ? std::string() : " " + std::string(clock(mark.sealedMs).utf8_str());
    std::string acked = acknowledged(m) ? "Acknowledged by " + m.to : std::string();
    std::string ack = awaitingAck(m) ? "Sent; no ack from " + m.to + " yet" : acked;
    switch (mark.mark)
    {
        case Mark::Awaiting:
            // Jeff, 2026-10-24: "Not yet verified by seal" while seals are
            // on; with them off (Mark::None) the words just leave it out.
            return (ack.empty() ? std::string("Sent") : ack)
                   + ". Not yet verified by seal (it goes after a minute's quiet)";
        case Mark::Sealed:
            if (!m.ours) return "Sealed: " + m.from + "'s seal lists it, and it matches";
            if (!acked.empty()) return acked + ", and sealed" + at;
            if (!mark.sentAgainFor.empty())
                return "Sealed" + at + ", and sent again for " + joinCalls(mark.sentAgainFor)
                       + (mark.sentAgainFor.size() == 1 ? ", who hasn't" : ", who haven't")
                       + " asked for a repair since, though silence doesn't prove they have it";
            return "Sealed" + at + ". No one has asked for a repair yet, though silence doesn't prove they all have it";
        case Mark::Repairing:
            return "Repairing: " + (mark.wanting.empty() ? std::string("asked for") : joinCalls(mark.wanting) + " asked for it")
                   + ", and it goes again";
        case Mark::Held:
            if (!acked.empty() && mark.holders.empty()) return acked + ", and sealed" + at;
            return (acked.empty() ? std::string() : acked + "; ") + "Held by " + joinCalls(mark.holders)
                   + ": a repair request under its seal" + at + " didn't ask for it";
        case Mark::Mismatch:
            return "Doesn't match " + m.from + "'s seal: a good copy is asked for";
        case Mark::None:
            break;
    }
    if (!ack.empty()) return ack;
    if (m.ours && !m.past && m.state == Message::State::Sent) return "Sent";
    return std::string();
}

// What a message's badge means: the words that were on the right of ours,
// and what its seal says. For the badge's tooltip and the greyed line atop
// its menu; empty when there is nothing to say.
std::string statusLine(const Message& m, const ragtime::broadcast::Seals::ItemMark* mark)
{
    if (m.ours)
    {
        std::string closed = m.past ? " (before Ragtime last closed)" : "";
        switch (m.state)
        {
            case State::Queued:
                return "Queued: waiting its turn, not yet given to the modem";
            case State::Sending:
                return "Sending: added to the modem's send queue; waiting for a clear channel";
            case State::Held:
                return "Held: in the modem's send queue; held until our session goes idle";
            case State::NotSent:
                return "Not sent: the modem dropped it, the link to it closed, or two minutes passed" + closed
                       + ". Right click to re-send";
            case State::Cancelled:
                return "Cancelled: taken back before it went" + closed;
            case State::Sent:
            case State::Acknowledged:
            case State::Heard:
                break;
        }
        if (m.past)
            return (m.state == State::Acknowledged ? "Acknowledged by " + m.to : std::string("Sent")) + closed;
    }
    return mark == nullptr ? std::string() : sealLine(m, *mark);
}

// A ping row's tooltip: what the figures are.
wxString pingTip(const Message& m)
{
    if (!m.ours)
        return "Its Ragtime asked how well we hear it; ours answered by itself, with our data2g-host's S/N "
               "for its ping, in the gear it pinged in.";
    if (m.state == State::Acknowledged)
        return "It hears us at: the S/N its data2g-host gave for our ping. We hear it at: ours for its pong. "
               "Both in dB in 2500 Hz, the gearshift's scale; the pong came back in the ping's gear.";
    if (m.unanswered)
        return "No pong yet: it may not have heard the ping, we may not have heard the pong, or its Ragtime is "
               "older than 0.2.0. A late pong still counts. Right click to ping again.";
    return "It answers by itself, in the same gear, with the S/N its host heard us at.";
}

// The Gear button's speedometer, as wide as fits over the gear's name
// with the button no taller than Send.
constexpr int GEAR_ICON_DIP = 30;

// Trims a button's padding off top and foot, so a tall picture fits it at the
// height of the buttons beside it. GTK pads an image button generously;
// elsewhere the padding is slim already.
void snugButton(wxWindow* button)
{
#if defined(__WXGTK3__)
    GtkWidget* widget = reinterpret_cast<GtkWidget*>(button->GetHandle());
    if (widget == nullptr || !GTK_IS_BUTTON(widget)) return;
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "button { padding: 0 4px; min-height: 0; }", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    button->InvalidateBestSize();
#else
    (void)button;
#endif
}

wxString clock(uint64_t ms)
{
    wxDateTime at((time_t)(ms / 1000));
    return at.Format(at.IsSameDate(wxDateTime::Today()) ? "%H:%M" : "%d %b %H:%M");
}

const int ID_REPLY = wxID_HIGHEST + 401;
const int ID_CANCEL = wxID_HIGHEST + 402;
const int ID_RESEND = wxID_HIGHEST + 403;
const int ID_COPY = wxID_HIGHEST + 404;
const int ID_SEAL_LINE = wxID_HIGHEST + 405;
const int ID_SAVE_WAV = wxID_HIGHEST + 406;
const int ID_FILE_ACTION = wxID_HIGHEST + 410;     // and FILE_MENU_IDS after it
const int ID_REACT = wxID_HIGHEST + 470;           // and REACTION_MENU_IDS after it

} // namespace

class MessageList : public ChatList
{
public:
    explicit MessageList(BroadcastPage* owner)
        : ChatList(owner)
        , owner_(owner)
    {
    }

    const Message* at(size_t n) const
    {
        const auto& all = owner_->chat_.messages();
        return n < all.size() ? &all[n] : nullptr;
    }

protected:
    void OnLinkClicked(size_t n, const wxHtmlLinkInfo& link) override
    {
        const Message* m = at(n);
        if (m == nullptr) return;
        if (uint8_t code = reactionHref(link.GetHref()))
        {
            // A chip: ours added to it, or, ours still waiting, taken back.
            bool takeBack = code == m->myWaiting && owner_->chat_.reactionWaiting(m->id);
            uint64_t id = m->id;
            BroadcastPage* owner = owner_;
            if (owner_->onReact_) owner_->CallAfter([owner, id, code, takeBack]() { owner->onReact_(id, takeBack ? 0 : code); });
            return;
        }
        const wxString href = link.GetHref();
        if (href.StartsWith("voice:"))
        {
            // A voice card: a live file's, or a file line's from its file.
            std::string key = m->file != 0 ? "b" + std::to_string(m->file) : m->path.empty() ? "" : "p" + m->path;
            if (key.empty() || !owner_->onVoice_) return;
            std::string what(href.utf8_str());
            BroadcastPage* owner = owner_;
            owner_->CallAfter([owner, key, what]() { owner->onVoice_(key, what); });
            return;
        }
        if (href == "pastpicture")
        {
            if (m->path.empty() || !owner_->onViewPast_) return;
            std::string path = m->path;
            BroadcastPage* owner = owner_;
            owner_->CallAfter([owner, path]() { owner->onViewPast_(path); });
            return;
        }
        if (m->file != 0 && owner_->onFileAction_)
            owner_->onFileAction_(m->file, std::string(href.utf8_str()));
    }

    wxString OnGetItemMarkup(size_t n) const override
    {
        return listText(OnGetItem(n), IsSelected(n));
    }

    wxString OnGetItem(size_t n) const override
    {
        const Message* m = at(n);
        if (m == nullptr) return wxString();
        bool picked = IsSelected(n);
        auto colour = [picked](const wxColour& c) {
            return picked ? wxString() : " color=\"" + htmlColour(c) + "\"";
        };
        if (m->kind == Message::Kind::Rule) return ruleItem(*m, picked);
        if (m->file != 0) return fileItem(*m, picked);
        if (m->kind == Message::Kind::File) return pastFileItem(*m, picked);
        if (m->kind == Message::Kind::Ping) return pingItem(*m, picked);
        const std::string& me = owner_->chat_.settings().callsign;
        // What came before Ragtime last closed reads in grey.
        wxString body = m->past ? colour(secondaryText()) : wxString();

        wxString to;
        if (!m->to.empty())
        {
            bool toMe = !me.empty() && m->to == me;
            to << "<font" << colour(m->past ? secondaryText() : accentText()) << ">@" << htmlEscape(m->to) << (toMe ? " (you)" : "")
               << "</font> ";
        }

        // The badge says the state; its tooltip says it in words (Jeff,
        // 2026-10-24: in place of the words on the right).
        using Mark = ragtime::broadcast::Seals::Mark;
        Mark mark = owner_->sealMark_ && !m->past ? owner_->sealMark_(*m).mark : Mark::None;
        wxString badge = badgeFor(*m, mark, this);
        // On the right, the gear it went (or goes) in, by its short name
        // (Jeff, 2026-10-24: in place of the state's words).
        wxString gear;
        if (!m->mode.empty())
            gear << "<font" << colour(secondaryText()) << ">"
                 << htmlEscape(owner_->nameMode_ ? owner_->nameMode_(m->mode) : m->mode) << "</font>";

        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"" << BADGE_ROOM << "\" valign=\"top\">" << badge << "</td>"
             << "<td valign=\"top\" nowrap><font" << colour(secondaryText()) << ">" << clock(m->atMs)
             << "</font></td>"
             << "<td valign=\"top\" width=\"100%\"><font" << body << "><b>" << htmlEscape(m->from) << "</b>&nbsp; "
             << to << htmlEscape(m->text) << "</font>";
        // Its reactions, as chips under it.
        wxString chips = reactionChipsHtml(m->reactions, m->myReaction, m->myWaiting, me, owner_->chat_.canReact(*m),
                                           owner_->chat_.reactionWaiting(m->id), picked);
        if (!chips.empty()) html << "<br>" << chips;
        html << "</td>"
             << "<td valign=\"top\" nowrap align=\"right\">" << gear << "</td>"
             << "</tr></table>";
        return html;
    }

private:
    // Where Ragtime last closed: what is above came before.
    wxString ruleItem(const Message& m, bool picked) const
    {
        wxString font = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"50%\" valign=\"middle\"><hr></td>"
             << "<td nowrap valign=\"middle\"><font size=\"-1\"" << font << ">Ragtime closed " << clock(m.atMs)
             << "</font></td>"
             << "<td width=\"50%\" valign=\"middle\"><hr></td>"
             << "</tr></table>";
        return html;
    }

    // A ping and its pong, or a station's ping to us: one line, greyed,
    // with its state's badge while it goes, Acknowledged's once the pong is
    // back, and the gear on the right.
    wxString pingItem(const Message& m, bool picked) const
    {
        wxString font = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
        wxString badge;
        if (m.ours)
        {
            switch (m.state)
            {
                case State::Queued:
                    badge = statusBadgeHtml(StatusBadge::Queued, this);
                    break;
                case State::Sending:
                    badge = statusBadgeHtml(StatusBadge::Sending, this);
                    break;
                case State::Held:
                    badge = statusBadgeHtml(StatusBadge::Held, this);
                    break;
                case State::NotSent:
                    badge = statusBadgeHtml(StatusBadge::NotSent, this);
                    break;
                case State::Cancelled:
                    badge = statusBadgeHtml(StatusBadge::Cancelled, this);
                    break;
                case State::Acknowledged:
                    badge = statusBadgeHtml(StatusBadge::Acknowledged, this);
                    break;
                case State::Sent:
                    if (!m.unanswered) badge = statusBadgeHtml(StatusBadge::Sent, this);
                    break;
                case State::Heard:
                    break;
            }
        }
        wxString gear;
        if (!m.mode.empty())
            gear << "<font" << font << ">"
                 << htmlEscape(owner_->nameMode_ ? owner_->nameMode_(m.mode) : m.mode) << "</font>";
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"" << BADGE_ROOM << "\" valign=\"top\">" << badge << "</td>"
             << "<td valign=\"top\" nowrap><font" << font << ">" << clock(m.atMs) << "</font></td>"
             << "<td valign=\"top\" width=\"100%\"><font" << font << "><i>"
             << htmlEscape(pingLine(m)) << "</i></font></td>"
             << "<td valign=\"top\" nowrap align=\"right\">" << gear << "</td>"
             << "</tr></table>";
        return html;
    }

    // A broadcast file from the log: one line, greyed, and how it ended.
    wxString pastFileItem(const Message& m, bool picked) const
    {
        wxString font = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
        wxString who = m.ours ? wxString("You broadcast") : htmlEscape(m.from) + " broadcast";
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"" << BADGE_ROOM << "\"></td>"
             << "<td valign=\"top\" nowrap><font" << font << ">" << clock(m.atMs) << "</font></td>"
             << "<td valign=\"top\" width=\"100%\"><font" << font << "><b>" << who << "</b>"
             << (m.voice ? " a voice message: " : m.picture ? " a picture: " : " a file: ") << htmlEscape(m.text)
             << "</font>";
        // A picture or a voice message kept, under its line from its file,
        // as Ragtime showed it before it closed.
        if (m.picture && !m.path.empty() && owner_->pastPictureHtml_)
        {
            wxString img = owner_->pastPictureHtml_(m.path, this, "pastpicture");
            if (!img.empty()) html << "<br>" << img;
        }
        if (m.voice && !m.path.empty() && owner_->voiceHtml_)
        {
            wxString card = owner_->voiceHtml_("p" + m.path, this, picked);
            if (!card.empty()) html << card;
        }
        html << "</td>"
             << "<td valign=\"top\" nowrap align=\"right\"><font" << font << ">" << htmlEscape(m.outcome)
             << "</font></td>"
             << "</tr></table>";
        return html;
    }

    // A broadcast file: who, the file, its state, and its links.
    wxString fileItem(const Message& m, bool picked) const
    {
        auto colour = [picked](const wxColour& c) {
            return picked ? wxString() : " color=\"" + htmlColour(c) + "\"";
        };
        FileCard card;
        if (!owner_->fileCard_ || !owner_->fileCard_(m.file, card)) card.what = m.text;
        wxColour c = secondaryText();
        if (card.good) c = goodText();
        if (card.bad) c = badText();
        wxString links;
        for (const auto& action : card.actions)
        {
            if (!links.empty()) links << " &middot; ";
            links << htmlLink(htmlEscape(action.first), htmlEscape(action.second), picked);
        }
        wxString who = m.ours ? wxString("You broadcast") : htmlEscape(m.from) + " broadcasts";
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\"><tr>"
             << "<td width=\"" << BADGE_ROOM << "\"></td>"
             << "<td valign=\"top\" nowrap><font" << colour(secondaryText()) << ">" << clock(m.atMs)
             << "</font></td>"
             << "<td valign=\"top\" width=\"100%\"><b>" << who << "</b>"
             << (card.voice ? " a voice message: " : card.picture ? " a picture: " : " a file: ") << htmlEscape(card.what);
        // A voice message's card, filling as it comes.
        if (card.voice && owner_->voiceHtml_)
        {
            wxString voice = owner_->voiceHtml_("b" + std::to_string(m.file), this, picked);
            if (!voice.empty()) html << voice;
        }
        // A picture shows as it stands, sharper as more of it comes.
        if (card.picture && owner_->pictureHtml_)
        {
            wxString img = owner_->pictureHtml_(m.file, this, "view");
            if (!img.empty()) html << "<br>" << img;
            // Its digest covers only the whole file, so a part of it can't
            // be checked against anything yet.
            if (!img.empty() && !m.ours && !card.good)
                html << "<br><font" << colour(secondaryText()) << "><i>As it has come so far, not yet verified</i></font>";
        }
        // The state under it, where a long one ("Stopped at 3 of 13 pieces.
        // Repairs open until ...") can wrap, then the links.
        html << "<br><font" << colour(c) << ">" << htmlEscape(card.state) << "</font>";
        if (!links.empty()) html << "&nbsp;&nbsp; " << links;
        html << "</td>"
             << "</tr></table>";
        return html;
    }

    BroadcastPage* owner_;
};

BroadcastPage::BroadcastPage(wxWindow* parent, const BroadcastChat& chat)
    : wxPanel(parent)
    , chat_(chat)
{
    list_ = new MessageList(this);
    followThemeBackground(list_, wxSYS_COLOUR_LISTBOX);
    scrollbarBeside(list_);
    paused_ = new wxStaticText(this, wxID_ANY,
                               PAUSED_LINE);
    followThemeText(paused_, secondaryText);
    paused_->Hide();

    compose_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | PHRASE_ENTRY_STYLE);
    compose_->SetHint("Message to everyone on D2G; start with @CALL for one station");
    compose_->SetMaxLength((unsigned long)MAX_TYPED);
    // A floppy disk rather than "File...", to save room (Jeff, 2026-10-23).
    file_ = new wxBitmapButton(this, wxID_ANY, floppyIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    file_->SetToolTip("Broadcast a file to everyone on D2G (300 KB at most), with repairs for stations that "
                      "miss pieces");
    // The picture button, just left of the floppy (Jeff, 2026-10-27).
    picture_ = new wxBitmapButton(this, wxID_ANY, pictureIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    picture_->SetToolTip("Broadcast a picture to everyone on D2G: shrunk and coded as JPEG 2000 or Cool-chic to fit "
                         "the time you allow, shown as it comes");
    // The microphone deck's button, left of the picture's.
    mic_ = new wxBitmapToggleButton(this, wxID_ANY, microphoneIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
    mic_->SetToolTip("The microphone deck: hold Push to talk to record a voice message for the net, or for the "
                     "station you're in a session with");
    // The gearshift: a speedometer with the gear's short name under it
    // ("A29") in small type, both drawn in the button's picture.
    gear_ = new wxBitmapButton(this, wxID_ANY, wxBitmapBundle(), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    snugButton(gear_);
    drawGearButton();
    send_ = new wxButton(this, wxID_ANY, "Send");
    file_->SetMinSize(wxSize(-1, std::max(file_->GetBestSize().y, send_->GetBestSize().y)));
    picture_->SetMinSize(wxSize(-1, std::max(picture_->GetBestSize().y, send_->GetBestSize().y)));
    mic_->SetMinSize(wxSize(-1, std::max(mic_->GetBestSize().y, send_->GetBestSize().y)));
    gear_->SetMinSize(wxSize(FromDIP(48), send_->GetBestSize().y));

    // Send, the file button, then the gearshift, as on the Session tab
    // (Jeff, 2026-10-23); Tab moves through them in the same order.
    mic_->MoveAfterInTabOrder(send_);
    picture_->MoveAfterInTabOrder(mic_);
    file_->MoveAfterInTabOrder(picture_);
    gear_->MoveAfterInTabOrder(file_);
    buttons_ = new wxBoxSizer(wxHORIZONTAL);
    buttons_->Add(send_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    buttons_->Add(mic_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    buttons_->Add(picture_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    buttons_->Add(file_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    buttons_->Add(gear_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    row_ = new wxBoxSizer(wxHORIZONTAL);
    row_->Add(compose_, 1, wxALIGN_CENTER_VERTICAL);
    row_->Add(buttons_, 0, wxEXPAND);
    under_ = new wxBoxSizer(wxHORIZONTAL);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(paused_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(row_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    sizer->Add(under_, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);
    sizer->AddSpacer(6);
    SetSizer(sizer);
    // In a narrow tab the Gear and Send buttons go under the line, so the
    // line keeps room to type.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        arrange(e.GetSize().x);
        e.Skip();
    });

    compose_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { send(); });
    compose_->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        showPhrases();
        e.Skip();
    });
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
    gear_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onGear_) onGear_();
    });
    file_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onBroadcastFile_) onBroadcastFile_();
    });
    picture_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onBroadcastPicture_) onBroadcastPicture_();
    });
    mic_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        // The deck says whether it is open (setVoiceDeckLit).
        mic_->SetValue(!mic_->GetValue());
        if (onVoiceDeck_) onVoiceDeck_();
    });
    // The picture, the floppy and the speedometer follow the theme's text colour, light or dark.
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
        const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
        file_->SetBitmap(floppyIcon(ink));
        picture_->SetBitmap(pictureIcon(ink));
        mic_->SetBitmap(microphoneIcon(ink));
        drawGearButton();
        e.Skip();
    });
    list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxPoint at = list_->ScreenToClient(e.GetPosition());
        int row = e.GetPosition() == wxDefaultPosition ? list_->GetSelection() : list_->VirtualHitTest(at.y);
        if (row != wxNOT_FOUND) list_->SetSelection(row);
        showMenu(row);
    });
    list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        const Message* m = list_->at((size_t)std::max(0, list_->VirtualHitTest(e.GetPosition().y)));
        wxString tip;
        if (m != nullptr && m->kind == Message::Kind::Text && list_->VirtualHitTest(e.GetPosition().y) != wxNOT_FOUND)
        {
            // Over the badge, what it means; elsewhere, when and how it went.
            if (e.GetPosition().x < list_->GetMargins().x + list_->FromDIP(BADGE_ROOM + 4))
            {
                SealMark mark;
                bool marked = sealMark_ && !m->past;
                if (marked) mark = sealMark_(*m);
                tip = wxString::FromUTF8(statusLine(*m, marked ? &mark : nullptr));
            }
            else
            {
                tip = wxDateTime((time_t)m->time).FormatISOCombined(' ') + wxString::Format(", item %u", m->seq);
                if (!m->mode.empty()) tip += ", " + wxString::FromUTF8(nameMode_ ? nameMode_(m->mode) : m->mode);
            }
        }
        if (m != nullptr && m->kind == Message::Kind::Ping && list_->VirtualHitTest(e.GetPosition().y) != wxNOT_FOUND)
            tip = pingTip(*m);
        list_->SetToolTip(tip);
        e.Skip();
    });
    refresh();
}

void BroadcastPage::setVoiceDeckLit(bool lit)
{
    if (mic_->GetValue() != lit) mic_->SetValue(lit);
}

void BroadcastPage::arrange(int width)
{
    bool stack = buttons_->GetMinSize().x + FromDIP(260) > width;
    if (stack == stacked_) return;
    (stacked_ ? under_ : row_)->Detach(buttons_);
    under_->Clear();    // its spacer
    if (stack)
    {
        under_->AddStretchSpacer();
        under_->Add(buttons_, 0, wxEXPAND | wxTOP, 6);
    }
    else
    {
        row_->Add(buttons_, 0, wxEXPAND);
    }
    stacked_ = stack;
    Layout();
}

void BroadcastPage::send()
{
    std::string typed(compose_->GetValue().utf8_str());
    if (onSend_ && onSend_(typed))
    {
        // Keep the address for the next line, as a reply goes on.
        std::string call, rest;
        if (BroadcastChat::addressee(typed, call, rest))
            address(call);
        else
            compose_->Clear();
    }
    compose_->SetFocus();
}

void BroadcastPage::refresh()
{
    uint64_t seals = sealsVersion_ ? sealsVersion_() : 0;
    if (shownVersion_ == chat_.version() && shownSeals_ == seals && list_->GetItemCount() == chat_.messages().size())
        return;
    shownVersion_ = chat_.version();
    shownSeals_ = seals;
    list_->showRows(chat_.messages().size());
}

void BroadcastPage::refreshVoices()
{
    list_->RefreshAll();
}

void BroadcastPage::setGearLabel(const wxString& label, const wxString& tooltip, double bitsPerSecond)
{
    gearName_ = label;
    gearSpeed_ = bitsPerSecond;
    drawGearButton();
    gear_->SetToolTip(tooltip);
    arrange(GetSize().x);
    Layout();
}

void BroadcastPage::drawGearButton()
{
    gear_->SetBitmap(gearButtonIcon(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT), needleOrange(),
                                    ragtime::gearbox::dialPosition(gearSpeed_), GetFont().Smaller(), gearName_,
                                    GEAR_ICON_DIP));
}

void BroadcastPage::setPaused(bool paused, const wxString& why)
{
    wxString line = why.empty() ? wxString(PAUSED_LINE) : why;
    if (paused_->IsShown() == paused && paused_->GetLabel() == line) return;
    paused_->SetLabel(line);
    paused_->Show(paused);
    Layout();
}

void BroadcastPage::address(const std::string& call)
{
    wxString text = "@" + wxString::FromUTF8(call) + " ";
    // GTK selects a line's text when it takes the focus, and typing would
    // replace the address: focus first, then the text, the cursor after it.
    compose_->SetFocus();
    compose_->ChangeValue(text);
    showPhrases();
    compose_->SetInsertionPointEnd();
    CallAfter([this]() {
        compose_->SelectNone();
        compose_->SetInsertionPointEnd();
    });
}

void BroadcastPage::showPhrases()
{
    std::string typed(compose_->GetValue().utf8_str());
    highlightPhrases(compose_, typed, ragtime::broadcast::BroadcastChat::phrases(typed));
}

void BroadcastPage::focusCompose()
{
    compose_->SetFocus();
}

void BroadcastPage::showMenu(int row)
{
    const Message* m = row == wxNOT_FOUND ? nullptr : list_->at((size_t)row);
    if (m == nullptr || m->kind == Message::Kind::Rule) return;
    if (m->file != 0 || m->kind == Message::Kind::File)
    {
        showFileMenu(*m);
        return;
    }
    if (m->kind == Message::Kind::Ping)
    {
        showPingMenu(*m);
        return;
    }
    uint64_t id = m->id;
    std::string from = m->from;
    wxString text = wxString::FromUTF8(m->text);

    wxMenu menu;
    SealMark mark;
    bool marked = sealMark_ && !m->past;
    if (marked) mark = sealMark_(*m);
    std::string seal = statusLine(*m, marked ? &mark : nullptr);
    if (!seal.empty())
    {
        menu.Append(ID_SEAL_LINE, wxString::FromUTF8(seal));
        menu.Enable(ID_SEAL_LINE, false);
        menu.AppendSeparator();
    }
    if (!m->ours) menu.Append(ID_REPLY, "Reply to " + wxString::FromUTF8(from));
    if (chat_.canReact(*m))
    {
        uint8_t current = m->myWaiting != 0 ? m->myWaiting : m->myReaction;
        menu.AppendSubMenu(reactionMenu(ID_REACT, current, chat_.reactionWaiting(id),
                                        [this, id](uint8_t code) {
                                            if (onReact_) onReact_(id, code);
                                        }),
                           "React");
    }
    if (m->ours && m->state == State::Queued) menu.Append(ID_CANCEL, "Cancel");
    if (chat_.canResend(*m)) menu.Append(ID_RESEND, "Re-send");
    if (menu.GetMenuItemCount() > 0) menu.AppendSeparator();
    menu.Append(ID_COPY, "Copy text");
    menu.Bind(wxEVT_MENU, [=](wxCommandEvent& e) {
        if (e.GetId() == ID_REPLY) address(from);
        if (e.GetId() == ID_CANCEL && onCancel_) onCancel_(id);
        if (e.GetId() == ID_RESEND && onResend_) onResend_(id);
        if (e.GetId() == ID_COPY && wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&menu);
}

void fillFileMenu(wxMenu& menu, const FileCard& card, int firstId, std::function<void(const std::string&)> act)
{
    // Ids in order: the title, the actions, then the submenu's items.
    std::vector<std::string> actionOf;
    auto next = [&](const std::string& action) {
        int id = firstId + (int)actionOf.size();
        actionOf.push_back(action);
        return id;
    };
    if (!card.title.empty())
    {
        int id = next("");
        menu.Append(id, wxString::FromUTF8(card.title));
        menu.Enable(id, false);
    }
    const auto& actions = card.menu.empty() ? card.actions : card.menu;
    for (const auto& action : actions)
    {
        if (actionOf.size() >= (size_t)FILE_MENU_IDS) break;
        int id = next(action.first);
        menu.Append(id, wxString::FromUTF8(action.second));
        if (action.first.empty()) menu.Enable(id, false);
    }
    wxMenu* sub = nullptr;
    if (!card.sub.empty())
    {
        sub = new wxMenu;
        for (const auto& item : card.sub)
        {
            if (actionOf.size() >= (size_t)FILE_MENU_IDS) break;
            int id = next(item.first);
            sub->AppendRadioItem(id, wxString::FromUTF8(item.second));
            if (item.first == card.subChosen) sub->Check(id, true);
        }
        menu.AppendSubMenu(sub, wxString::FromUTF8(card.subLabel));
    }
    auto handler = [actionOf, firstId, act](wxCommandEvent& e) {
        int index = e.GetId() - firstId;
        if (index < 0 || (size_t)index >= actionOf.size() || actionOf[(size_t)index].empty())
        {
            e.Skip();
            return;
        }
        act(actionOf[(size_t)index]);
    };
    int last = firstId + FILE_MENU_IDS - 1;
    menu.Bind(wxEVT_MENU, handler, firstId, last);
    if (sub != nullptr) sub->Bind(wxEVT_MENU, handler, firstId, last);
}

void BroadcastPage::showPingMenu(const Message& m)
{
    uint64_t id = m.id;
    std::string call = m.ours ? m.to : m.from;
    wxString line = wxString::FromUTF8(pingLine(m));
    std::string why = whyNoPing_ ? whyNoPing_(call) : std::string("not here");

    wxMenu menu;
    if (m.ours && m.state == State::Queued) menu.Append(ID_CANCEL, "Cancel");
    // Never again by itself: the operator's to ask (Jeff's proposal,
    // 2026-10-22).
    wxString again = (m.ours ? "Ping " : "Ping back ") + wxString::FromUTF8(call) + (m.ours ? " again" : "");
    if (!why.empty()) again += " (" + wxString::FromUTF8(why) + ")";
    menu.Append(ID_RESEND, again);
    menu.Enable(ID_RESEND, why.empty());
    menu.AppendSeparator();
    menu.Append(ID_COPY, "Copy text");
    menu.Bind(wxEVT_MENU, [=](wxCommandEvent& e) {
        if (e.GetId() == ID_CANCEL && onCancel_) onCancel_(id);
        if (e.GetId() == ID_RESEND && onPing_) onPing_(call);
        if (e.GetId() == ID_COPY && wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(line));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&menu);
}

void BroadcastPage::showFileMenu(const Message& m)
{
    uint64_t file = m.file;
    FileCard card;
    if (fileCard_ && file != 0) fileCard_(file, card);
    wxString name = wxString::FromUTF8(m.text);

    wxMenu menu;
    fillFileMenu(menu, card, ID_FILE_ACTION, [this, file](const std::string& action) {
        if (onFileAction_) onFileAction_(file, action);
    });
    // A voice message: its sound as a WAV file, for keeping or playing
    // anywhere (a live file's, or a kept one's from its file).
    std::string voiceKey;
    if (card.voice && file != 0)
        voiceKey = "b" + std::to_string(file);
    else if (m.voice && !m.path.empty())
        voiceKey = "p" + m.path;
    if (!voiceKey.empty() && onVoice_)
    {
        if (menu.GetMenuItemCount() > 0) menu.AppendSeparator();
        menu.Append(ID_SAVE_WAV, "Save as &WAV...");
        menu.Bind(wxEVT_MENU, [this, voiceKey](wxCommandEvent&) {
            std::string key = voiceKey;
            CallAfter([this, key]() { onVoice_(key, "voice:wav:0"); });
        }, ID_SAVE_WAV);
    }
    if (menu.GetMenuItemCount() > 0) menu.AppendSeparator();
    menu.Append(ID_COPY, "Copy file name");
    menu.Bind(wxEVT_MENU, [=](wxCommandEvent& e) {
        if (e.GetId() == ID_COPY && wxTheClipboard->Open())
        {
            wxTheClipboard->SetData(new wxTextDataObject(name));
            wxTheClipboard->Close();
        }
    }, ID_COPY);
    PopupMenu(&menu);
}
