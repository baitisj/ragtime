#include "ConnectionBar.h"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include "Html.h"
#include "ToggleSwitch.h"
#include "TxLamp.h"

namespace
{

wxStaticLine* divider(wxWindow* parent)
{
    return new wxStaticLine(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL);
}

} // namespace

ConnectionBar::ConnectionBar(wxWindow* parent)
    : wxPanel(parent)
{
    callsign_ = new wxStaticText(this, wxID_ANY, "No callsign");
    callsign_->SetFont(callsign_->GetFont().Bold());
    callsign_->SetToolTip("Your callsign: Settings > Station");
    auto* group = new wxStaticText(this, wxID_ANY, "Group D2G");
    group->SetToolTip("The broadcast group Ragtime opens on data2g-host");
    dial_ = new wxStaticText(this, wxID_ANY, "");
    dial_->Hide();

    narrow = new ToggleSwitch(this, wxID_ANY, "500 Hz");
    narrow->SetToolTip("On: data2g-host uses only 500 Hz modes (BW500). Off: any width (BW2300)");
    acceptSessions = new wxCheckBox(this, wxID_ANY, "Accept sessions");
    acceptSessions->SetToolTip("Whether other stations can call you (LISTEN ON or OFF)");
    pause = new wxToggleButton(this, wxID_ANY, "Pause sending", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pause->SetToolTip("Hold new broadcasts as Queued; a burst data2g-host already has still goes");

    // Auto ALC needs data2g-host to take a TX level command (an ask for
    // Andrew), so it waits, greyed, until then. The meter beside it shows
    // the rig's ALC when Ragtime opens the rig with Hamlib (setAlc).
    autoAlc = new wxCheckBox(this, wxID_ANY, "Auto ALC");
    alcMeter = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(60), FromDIP(10)),
                           wxGA_HORIZONTAL | wxGA_SMOOTH);
    alcReading = new wxStaticText(this, wxID_ANY, "100%", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    alcReading->SetMinSize(alcReading->GetBestSize());
    alcReading->SetLabel(wxString::FromUTF8("\xe2\x80\x94"));
    autoAlc->SetToolTip("Keeps transmit drive just under the rig's ALC. It waits for data2g-host to take a TX "
                        "level command, since Ragtime never touches the sound card.");
    autoAlc->Disable();
    setAlc(false, 0, wxString::FromUTF8("\xe2\x80\x94"),
           "The rig's ALC while transmitting, when Ragtime opens the rig with Hamlib (Settings > Rig).", false);
    tx = new TxLamp(this);

    left_ = new wxBoxSizer(wxHORIZONTAL);
    auto add = [this](wxSizer* to, wxWindow* w, int left) {
        to->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(left));
    };
    add(left_, callsign_, 8);
    add(left_, group, 12);
    add(left_, dial_, 12);
    left_->Add(divider(this), 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
    add(left_, narrow, 8);
    add(left_, acceptSessions, 14);
    add(left_, pause, 12);

    right_ = new wxBoxSizer(wxHORIZONTAL);
    add(right_, autoAlc, 8);
    add(right_, alcMeter, 6);
    add(right_, alcReading, 6);
    add(right_, tx, 12);
    right_->AddSpacer(FromDIP(8));

    // One row, the TX light at the right end; on a narrow window the Auto
    // ALC and TX group drops to a second row rather than off the edge.
    first_ = new wxBoxSizer(wxHORIZONTAL);
    first_->Add(left_, 0, wxEXPAND);
    first_->AddStretchSpacer();
    first_->Add(right_, 0, wxEXPAND);
    second_ = new wxBoxSizer(wxHORIZONTAL);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(first_, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    outer->Add(second_, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
    SetSizer(outer);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        arrange(e.GetSize().x);
        e.Skip();
    });

    pause->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        lightToggle(pause);
        e.Skip();
    });
}

void ConnectionBar::arrange(int width)
{
    bool fits = left_->GetMinSize().x + right_->GetMinSize().x + FromDIP(16) <= width;
    if (fits && twoRows_)
    {
        second_->Detach(right_);
        second_->Clear();   // the spacer
        first_->Add(right_, 0, wxEXPAND);
        twoRows_ = false;
    }
    else if (!fits && !twoRows_)
    {
        first_->Detach(right_);
        second_->AddStretchSpacer();
        second_->Add(right_, 0, wxEXPAND);
        twoRows_ = true;
    }
    else
    {
        return;
    }
    GetParent()->Layout();
}

void ConnectionBar::setCallsign(const wxString& callsign)
{
    callsign_->SetLabel(callsign.empty() ? wxString("No callsign") : callsign);
    Layout();
}

void ConnectionBar::setAlc(bool readable, int percent, const wxString& text, const wxString& tip, bool warn)
{
    for (wxWindow* w : {(wxWindow*)alcMeter, (wxWindow*)alcReading})
    {
        if (w->IsEnabled() != readable) w->Enable(readable);
        if (w->GetToolTipText() != tip) w->SetToolTip(tip);
    }
    percent = std::max(0, std::min(100, percent));
    if (alcMeter->GetValue() != percent) alcMeter->SetValue(percent);
    if (alcReading->GetLabel() != text) alcReading->SetLabel(text);
    if (warn != alcWarn_)
    {
        alcWarn_ = warn;
        alcReading->SetForegroundColour(warn ? badText() : wxNullColour);
        alcReading->Refresh();
    }
}

void ConnectionBar::setDial(const wxString& text, const wxString& tip, bool held)
{
    if (text.empty())
    {
        if (!dial_->IsShown()) return;
        dial_->Hide();
        Layout();
        arrange(GetSize().x);
        return;
    }
    if (dial_->GetToolTipText() != tip) dial_->SetToolTip(tip);
    if (dial_->IsShown() && dial_->GetLabel() == text && held_ == held) return;
    held_ = held;
    dial_->SetLabel(text);
    dial_->SetForegroundColour(held ? badText() : wxNullColour);
    dial_->Show();
    Layout();
    arrange(GetSize().x);
    GetParent()->Layout();
}

void ConnectionBar::setNarrow(bool on)
{
    narrow->SetValue(on);
}

void ConnectionBar::setPaused(bool on)
{
    pause->SetValue(on);
    lightToggle(pause);
}

// Toggles that are on turn blue (docs/MOCKUPS.md, "Look").
void ConnectionBar::lightToggle(wxToggleButton* button)
{
    if (button->GetValue())
    {
        button->SetBackgroundColour(ACCENT_BLUE);
        button->SetForegroundColour(*wxWHITE);
    }
    else
    {
        button->SetBackgroundColour(wxNullColour);
        button->SetForegroundColour(wxNullColour);
    }
    button->Refresh();
}
