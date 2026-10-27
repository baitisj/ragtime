#include "PictureDialog.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
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
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    view_ = new PictureView(this, FromDIP(PREVIEW_DIP), false);
    view_->setNote("Reading the picture...");
    sizer->Add(view_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // As it comes: the far end sees the whole picture from the first part,
    // sharper as the rest arrives.
    auto* partialRow = new wxBoxSizer(wxHORIZONTAL);
    partialRow->Add(new wxStaticText(this, wxID_ANY, "As it comes:"), 0, wxALIGN_CENTER_VERTICAL);
    partial_ = new wxSlider(this, wxID_ANY, 100, 1, 100);
    partial_->SetToolTip("What the far end sees part of the way: any first part of the picture shows the whole "
                         "of it, softer");
    partialRow->Add(partial_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    partialText_ = new wxStaticText(this, wxID_ANY, "");
    partialText_->SetForegroundColour(secondaryText());
    sizer->Add(partialRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
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
    partial_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { showPartial(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { remake(); });
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        timer_.Stop();
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
    if (!target_.mode.valid()) return 0;
    if (target_.broadcast) return picture::broadcastBytesFor(seconds, name_, target_.callsign, target_.mode);
    return picture::sessionBytesFor(seconds, picture::sessionBytesPerSecond(target_.mode, target_.measuredRate));
}

double PictureDialog::airSeconds(uint64_t bytes) const
{
    if (!target_.mode.valid()) return 0.0;
    if (target_.broadcast)
        return ragtime::broadcast::estimateFile(bytes, name_, target_.callsign, target_.mode).airSeconds;
    double rate = picture::sessionBytesPerSecond(target_.mode, target_.measuredRate);
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
    wxString gear = target_.gear.empty() ? wxString("this gear") : utf8(target_.gear);
    if (!target_.mode.valid())
    {
        air_->SetLabel(target_.broadcast ? "data2g-host hasn't listed its gears yet: connect, then try again."
                                         : "The session's gear isn't known yet.");
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
                               : wxString::Format("about %s in the session in %s%s", utf8(secondsText(air)), gear,
                                                  target_.measuredRate > 0 ? wxString(", at the rate it moves now")
                                                                           : wxString());
            air_->SetLabel(wxString::Format("%s in %d quality layer%s: %s", utf8(bytesText(coded_.bytes.size())),
                                            coded_.layers, coded_.layers == 1 ? "" : "s", how));
            air_->SetForegroundColour(GetForegroundColour());
        }
    }
    if (target_.broadcast)
    {
        double seal = target_.mode.valid() ? picture::sealSeconds(target_.callsign, target_.mode) : 0.0;
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
    if (whole_.IsOk())
        showPartial();
    else
    {
        view_->setImage(toWxImage(shrunk_), false);
        partialText_->SetLabel("");
    }
    // Long lines wrap at the preview's width, and the dialog grows to fit
    // them.
    for (wxStaticText* line : {air_, seal_}) line->Wrap(view_->GetSize().x);
    Layout();
    wxSize room = GetSizer()->GetMinSize();
    if (room.y > GetClientSize().y) SetClientSize(GetClientSize().x, room.y);
}

void PictureDialog::showPartial()
{
    if (coded_.bytes.empty()) return;
    int percent = partial_->GetValue();
    if (percent >= 100)
    {
        view_->setImage(whole_);
        partialText_->SetLabel("Whole: as it looks once all of it has come.");
        return;
    }
    size_t bytes = std::max<size_t>(1, coded_.bytes.size() * (size_t)percent / 100);
    picture::Image decoded;
    std::string error;
    if (!ragtime::image::decode(coded_.bytes.data(), bytes, decoded, error))
    {
        partialText_->SetLabel(wxString::Format("At %d%%: nothing to show yet.", percent));
        return;
    }
    view_->setImage(toWxImage(decoded));
    partialText_->SetLabel(wxString::Format("At %d%%, %s: about %s in.", percent, utf8(bytesText(bytes)),
                                            utf8(secondsText(airSeconds(bytes)))));
}
