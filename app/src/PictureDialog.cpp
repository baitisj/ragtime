#include "PictureDialog.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/colour.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include "Html.h"
#include "PictureView.h"
#include "Pictures.h"
#include "Prefs.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/gearbox/Gearbox.h"

namespace picture = ragtime::picture;

namespace
{

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

std::string bytesText(uint64_t bytes)
{
    return ragtime::files::groupedNumber(bytes) + " bytes";
}

std::string secondsText(double seconds)
{
    return ragtime::gearbox::formatDuration(seconds);
}

// The preview's room: the dialog fits a 1366 × 768 screen.
const wxSize PREVIEW_DIP(480, 300);

} // namespace

PictureDialog::PictureDialog(wxWindow* parent, const PictureTarget& target, const std::string& path)
    : wxDialog(parent, wxID_ANY,
               target.broadcast ? wxString("Broadcast a picture to D2G")
                                : "Send a picture to " + utf8(target.peer),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , target_(target)
    , timer_(this)
    , play_(this)
{
    if (!target_.gears.empty()) gear_ = target_.gears.front();
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    view_ = new PictureView(this, FromDIP(PREVIEW_DIP), false);
    view_->setNote("Reading the picture...");
    sizer->Add(view_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    // Under it, how much has come of the whole as it plays, "1.2k / 4.0k"
    // (Jeff, 2026-10-29).
    sizeText_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    sizeText_->SetFont(sizeText_->GetFont().Bold());
    sizer->Add(sizeText_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // The preview plays the picture arriving, each burst's end and steps
    // of a few percent between, as the far end sees it build up (the
    // codestream is progressive: each step the whole picture, sharper),
    // holds on the whole and goes round again (Jeff, 2026-10-27). The
    // slider sets how fast.
    auto* speedRow = new wxBoxSizer(wxHORIZONTAL);
    speedRow->Add(new wxStaticText(this, wxID_ANY, "Playback speed:"), 0, wxALIGN_CENTER_VERTICAL);
    speed_ = new wxSlider(this, wxID_ANY, 3, 0, MAX_SPEED_STEP);
    speed_->SetToolTip("How fast the preview plays the picture arriving: from real time to 64 times as fast. "
                       "Only the preview: it changes nothing that is sent");
    speedRow->Add(speed_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    speedText_ = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("64\xc3\x97"), wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
    speedText_->SetMinSize(wxSize(speedText_->GetTextExtent(wxString::FromUTF8("64\xc3\x97")).x + FromDIP(4), -1));
    speedRow->Add(speedText_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    partialText_ = new wxStaticText(this, wxID_ANY, "");
    partialText_->SetForegroundColour(secondaryText());
    sizer->Add(speedRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(partialText_, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
    file_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    fileRow->Add(file_, 1, wxALIGN_CENTER_VERTICAL);
    auto* another = new wxButton(this, wxID_ANY, "Another picture...");
    fileRow->Add(another, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    sizer->Add(fileRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    auto* grid = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
    side_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, picture::MIN_SIDE,
                           picture::MAX_SIDE, loadInt(keys::PICTURE_SIDE, picture::DEFAULT_MAX_SIDE));
    side_->SetToolTip("The most pixels across or down, whichever is longer; a smaller picture keeps its own size");
    seconds_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                              picture::MIN_SECONDS, picture::MAX_SECONDS,
                              loadInt(keys::PICTURE_SECONDS, picture::DEFAULT_SECONDS));
    seconds_->SetToolTip("The most airtime the picture may take in the gear it goes in. It is coded to fit, and "
                         "goes in less when it needs less");
    grid->Add(new wxStaticText(this, wxID_ANY, "Longest side:"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(side_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, "pixels"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, "Airtime at most:"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(seconds_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, "seconds"), 0, wxALIGN_CENTER_VERTICAL);
    // The gear: on D2G the one it goes in; in a session only what it is
    // planned for, as Data2G picks the session's gear burst by burst.
    gears_ = new wxChoice(this, wxID_ANY);
    for (const PictureGear& g : target_.gears) gears_->Append(utf8(g.label));
    if (!target_.gears.empty()) gears_->SetSelection(0);
    gears_->Enable(target_.gears.size() > 1);
    gears_->SetToolTip(target_.broadcast
                           ? wxString("The gear the picture goes in. The first is the one in effect; another "
                                      "sends this picture in that gear")
                           : wxString("The gear the picture is planned for. Data2G picks the session's gear "
                                      "burst by burst, from how each end hears the other, so this only sizes the "
                                      "picture: the first is from how we have heard ")
                                 + utf8(target_.peer)
                                 + " over the last few minutes, the path back being much the same");
    grid->Add(new wxStaticText(this, wxID_ANY, target_.broadcast ? "Gear:" : "Plan for:"), 0,
              wxALIGN_CENTER_VERTICAL);
    grid->Add(gears_, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    size_ = new wxStaticText(this, wxID_ANY, "");
    air_ = new wxStaticText(this, wxID_ANY, "");
    seal_ = new wxStaticText(this, wxID_ANY, "");
    seal_->SetForegroundColour(secondaryText());
    sizer->Add(size_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(air_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    sizer->Add(seal_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* cancel = new wxButton(this, wxID_CANCEL, "Cancel");
    send_ = new wxButton(this, wxID_OK, target_.broadcast ? "Broadcast" : "Send");
    send_->SetDefault();
    buttons->Add(cancel, 0);
    buttons->Add(send_, 0, wxLEFT, 8);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 12);
    SetSizer(sizer);

    another->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose(); });
    side_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { later(); });
    side_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { later(); });
    seconds_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { later(); });
    seconds_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { later(); });
    speed_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        speedPicked_ = true;
        showSpeed();
        // The step on screen waits the new speed's time.
        if (!steps_.empty()) play_.StartOnce(stepMs());
    });
    gears_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        int n = gears_->GetSelection();
        if (n < 0 || n >= (int)target_.gears.size()) return;
        gear_ = target_.gears[(size_t)n];
        later();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { remake(); }, timer_.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { nextStep(); }, play_.GetId());
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        timer_.Stop();
        play_.Stop();
        remake();
        if (coded_.bytes.empty()) return;
        saveInt(keys::PICTURE_SIDE, side_->GetValue());
        saveInt(keys::PICTURE_SECONDS, seconds_->GetValue());
        EndModal(wxID_OK);
    });

    Fit();
    // Every dialog fits a 1366 × 768 screen.
    SetMinSize(GetSize());
    CentreOnParent();
    if (load(path)) remake();
}

void PictureDialog::choose()
{
    wxFileDialog pick(this, "Pick a picture", wxFileName(utf8(path_)).GetPath(), "", pictureWildcard(),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() != wxID_OK) return;
    if (load(std::string(pick.GetPath().utf8_str()))) remake();
}

bool PictureDialog::load(const std::string& path)
{
    picture::Image image;
    std::string error;
    {
        wxBusyCursor busy;
        if (!loadPicture(path, image, error))
        {
            wxMessageBox(wxFileName(utf8(path)).GetFullName() + ": " + utf8(error), GetTitle(),
                         wxOK | wxICON_INFORMATION, this);
            if (picked_.empty())
            {
                view_->setNote("No picture");
                send_->Enable(false);
            }
            return false;
        }
    }
    path_ = path;
    picked_ = std::move(image);
    shrunk_ = picture::Image();
    shrunkSide_ = 0;
    name_ = picture::pictureName(path);
    file_->SetLabel(wxString::Format(wxString::FromUTF8("%s, %d \xc3\x97 %d"), wxFileName(utf8(path)).GetFullName(), picked_.width,
                                     picked_.height));
    file_->SetToolTip(utf8(path));
    return true;
}

void PictureDialog::later()
{
    timer_.StartOnce(350);
}

uint64_t PictureDialog::budget(double seconds) const
{
    if (!gear_.mode.valid()) return 0;
    if (target_.broadcast) return picture::broadcastBytesFor(seconds, name_, target_.callsign, gear_.mode);
    return picture::sessionBytesFor(seconds, picture::sessionBytesPerSecond(gear_.mode, 0.0));
}

double PictureDialog::airSeconds(uint64_t bytes) const
{
    if (!gear_.mode.valid()) return 0.0;
    if (target_.broadcast)
        return ragtime::broadcast::estimateFile(bytes, name_, target_.callsign, gear_.mode).airSeconds;
    double rate = picture::sessionBytesPerSecond(gear_.mode, 0.0);
    return rate > 0 ? ((double)bytes * (256.0 + 18.0) / 256.0 + 40.0) / rate : 0.0;
}

void PictureDialog::remake()
{
    if (picked_.empty()) return;
    wxBusyCursor busy;
    int side = std::max(picture::MIN_SIDE, std::min(picture::MAX_SIDE, side_->GetValue()));
    if (shrunk_.empty() || shrunkSide_ != side)
    {
        int w = 0, h = 0;
        picture::fitWithin(picked_.width, picked_.height, side, w, h);
        shrunk_ = picture::downscale(picked_, w, h);
        if (shrunk_.channels == 3 && picture::isGrey(shrunk_)) shrunk_ = picture::toGrey(shrunk_);
        shrunkSide_ = side;
    }
    const wxString colour = shrunk_.channels == 1 ? "grey" : "colour";
    size_->SetLabel(wxString::Format(wxString::FromUTF8("%d \xc3\x97 %d, %s"), shrunk_.width, shrunk_.height, colour));

    coded_ = ragtime::image::Coded();
    whole_ = wxImage();
    double seconds = seconds_->GetValue();
    uint64_t most = budget(seconds);
    wxString gear = gear_.words.empty() ? wxString("this gear") : utf8(gear_.words);
    if (!gear_.mode.valid())
    {
        air_->SetLabel("data2g-host hasn't listed its gears yet: connect, then try again.");
        air_->SetForegroundColour(badText());
    }
    else if (most == 0)
    {
        air_->SetLabel(wxString::Format("Too slow: in %s not even a small picture fits in %s. Allow more time, "
                                        "or pick a faster gear.",
                                        gear, utf8(secondsText(seconds))));
        air_->SetForegroundColour(badText());
    }
    else
    {
        std::string error;
        if (!ragtime::image::encode(shrunk_, most, coded_, error))
        {
            coded_ = ragtime::image::Coded();
            air_->SetLabel(utf8(error) + " Allow more time, or a smaller side.");
            air_->SetForegroundColour(badText());
        }
        else
        {
            picture::Image decoded;
            if (ragtime::image::decode(coded_.bytes.data(), coded_.bytes.size(), decoded, error))
                whole_ = toWxImage(decoded);
            double air = airSeconds(coded_.bytes.size());
            wxString how = target_.broadcast
                               ? wxString::Format("about %s on the air in %s", utf8(secondsText(air)), gear)
                               : wxString::Format("about %s in the session in %s", utf8(secondsText(air)), gear);
            air_->SetLabel(wxString::Format("%s in %d quality layer%s: %s", utf8(bytesText(coded_.bytes.size())),
                                            coded_.layers, coded_.layers == 1 ? "" : "s", how));
            air_->SetForegroundColour(GetForegroundColour());
        }
    }
    if (target_.broadcast)
    {
        double seal = gear_.mode.valid() ? picture::sealSeconds(target_.callsign, gear_.mode) : 0.0;
        seal_->SetLabel(seal > 0 ? wxString::Format("Then its seal: about %s more, then repairs for stations that "
                                                    "miss pieces, which take airtime too.",
                                                    utf8(secondsText(seal)))
                                 : wxString("Then its seal, and repairs for stations that miss pieces."));
    }
    else
    {
        seal_->SetLabel(utf8(target_.peer) + " sees it as it comes, and can stop it with the stop sign once "
                                              "they have enough.");
    }
    send_->Enable(!coded_.bytes.empty());
    play_.Stop();
    steps_.clear();
    frames_.clear();
    if (whole_.IsOk())
    {
        steps_ = target_.broadcast
                     ? picture::broadcastArrival(coded_.bytes.size(), name_, target_.callsign, gear_.mode)
                     : picture::sessionArrival(coded_.bytes.size(), gear_.mode);
        frames_.resize(steps_.size());
        // Until the operator picks a speed, one that plays it in about 8 s.
        if (!speedPicked_ && !steps_.empty())
        {
            int step = 0;
            while (step < MAX_SPEED_STEP && steps_.back().seconds / (double)(1 << step) > 8.0) step++;
            speed_->SetValue(step);
        }
        showSpeed();
        step_ = 0;
        showStep();
    }
    else
    {
        view_->setImage(toWxImage(shrunk_), false);
        partialText_->SetLabel("");
        sizeText_->SetLabel("");
    }
    // Long lines wrap at the preview's width, and the dialog grows to fit
    // them.
    for (wxStaticText* line : {air_, seal_}) line->Wrap(view_->GetSize().x);
    Layout();
    wxSize room = GetSizer()->GetMinSize();
    if (room.y > GetClientSize().y) SetClientSize(GetClientSize().x, room.y);
}

wxImage PictureDialog::blank() const
{
    wxImage image(whole_.GetWidth(), whole_.GetHeight());
    wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    image.SetRGB(wxRect(0, 0, image.GetWidth(), image.GetHeight()), face.Red(), face.Green(), face.Blue());
    return image;
}

wxString PictureDialog::farEnd(bool capital) const
{
    if (!target_.broadcast) return utf8(target_.peer);
    return capital ? wxString("A listener") : wxString("a listener");
}

void PictureDialog::showSpeed()
{
    speedText_->SetLabel(wxString::Format(wxString::FromUTF8("%d\xc3\x97"), 1 << speed_->GetValue()));
}

int PictureDialog::stepMs() const
{
    if (steps_.empty()) return 1000;
    // At the whole picture, a pause before it goes round again.
    if (step_ + 1 >= steps_.size()) return HOLD_MS;
    double seconds = (steps_[step_ + 1].seconds - steps_[step_].seconds) / (double)(1 << speed_->GetValue());
    return std::max(MIN_STEP_MS, std::min(MAX_STEP_MS, (int)std::lround(seconds * 1000.0)));
}

void PictureDialog::nextStep()
{
    if (steps_.empty()) return;
    step_ = step_ + 1 >= steps_.size() ? 0 : step_ + 1;
    showStep();
}

void PictureDialog::showStep()
{
    if (step_ >= steps_.size()) return;
    const picture::ArrivalStep& at = steps_[step_];
    bool whole = step_ + 1 == steps_.size();
    if (!frames_[step_].IsOk())
    {
        if (whole)
            frames_[step_] = whole_;
        else
        {
            picture::Image decoded;
            std::string error;
            if (ragtime::image::decode(coded_.bytes.data(), (size_t)at.bytes, decoded, error))
                frames_[step_] = toWxImage(decoded);
        }
    }
    int percent = (int)(100 * at.bytes / std::max<uint64_t>(1, coded_.bytes.size()));
    wxString when = utf8(secondsText(at.seconds));
    int bursts = steps_.back().burst;
    sizeText_->SetLabel(utf8(picture::comeText(at.bytes, coded_.bytes.size())));
    if (whole)
        partialText_->SetLabel(wxString::Format("%s has all of it, %s in.", farEnd(true), when));
    else if (!frames_[step_].IsOk())
        partialText_->SetLabel(wxString::Format("%s in, %s has nothing to show yet.", when, farEnd()));
    else if (at.burst != 0)
        partialText_->SetLabel(wxString::Format("What %s sees after burst %d of %d, %s in: %d%%.", farEnd(),
                                                at.burst, bursts, when, percent));
    else
        partialText_->SetLabel(wxString::Format("%d%% of it, %s in (%s sees it as each burst ends).", percent, when,
                                                farEnd()));
    // Nothing to show yet: a blank the picture's size, not the last frame
    // left standing (the whole picture, when it goes round again).
    if (frames_[step_].IsOk())
        view_->setImage(frames_[step_]);
    else if (whole_.IsOk())
        view_->setImage(blank());
    play_.StartOnce(stepMs());
}
