#include "PictureFrame.h"

#include <algorithm>
#include <cmath>

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include "AppIcon.h"
#include "Html.h"
#include "PictureView.h"
#include "ragtime/files/FileRows.h"

namespace
{

// The slider runs in hundredths of a doubling: -432 is 5%, 400 is 1600%.
int sliderFor(double scale)
{
    return (int)std::lround(100.0 * std::log2(scale));
}

double scaleFor(int position)
{
    return std::pow(2.0, position / 100.0);
}

} // namespace

PictureFrame::PictureFrame(wxWindow* parent, const wxString& title, const std::string& suggestedName)
    : wxFrame(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
    , suggestedName_(suggestedName)
{
    SetIcons(appIcons());
    auto* panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    view_ = new PictureView(panel, FromDIP(wxSize(400, 300)), true);
    view_->setNote("Nothing to show yet");
    view_->SetToolTip("The wheel zooms, a drag moves it, a double click goes between Fit and 1:1");
    sizer->Add(view_, 1, wxEXPAND);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* fit = new wxButton(panel, wxID_ANY, "Fit", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* one = new wxButton(panel, wxID_ANY, "1:1", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    zoom_ = new wxSlider(panel, wxID_ANY, 0, sliderFor(PictureView::MIN_SCALE), sliderFor(PictureView::MAX_SCALE));
    zoom_->SetToolTip("Zoom");
    zoomText_ = new wxStaticText(panel, wxID_ANY, "100%", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
    zoomText_->SetMinSize(wxSize(zoomText_->GetTextExtent("1600%").x + FromDIP(4), -1));
    auto* saveAs = new wxButton(panel, wxID_ANY, "Save as...");
    row->Add(fit, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(one, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    row->Add(zoom_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    row->Add(zoomText_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    row->Add(saveAs, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    about_ = new wxStaticText(panel, wxID_ANY, "");
    about_->SetForegroundColour(secondaryText());
    sizer->Add(about_, 0, wxEXPAND | wxALL, 8);
    panel->SetSizer(sizer);

    view_->onZoom([this](double scale) { zoomed(scale); });
    fit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { view_->fit(); });
    one->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { view_->zoomTo(1.0); });
    zoom_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { view_->zoomTo(scaleFor(zoom_->GetValue())); });
    saveAs->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (onClosed_) onClosed_();
        e.Skip();
    });

    // Room for a 640-pixel picture at 1:1, within a 1366 × 768 screen.
    SetClientSize(FromDIP(wxSize(680, 600)));
    SetMinClientSize(FromDIP(wxSize(360, 300)));
    CentreOnParent();
}

void PictureFrame::show(const wxImage& image, int width, int height, uint64_t bytes, uint64_t size)
{
    if (!image.IsOk()) return;
    image_ = image;
    view_->setImage(image);
    wxString line = wxString::Format(wxString::FromUTF8("%d \xc3\x97 %d"), width > 0 ? width : image.GetWidth(),
                                     height > 0 ? height : image.GetHeight());
    if (size > 0 && bytes < size)
        line += wxString::Format(", %s of %s bytes so far (%d%%): sharper as the rest comes",
                                 wxString::FromUTF8(ragtime::files::groupedNumber(bytes)),
                                 wxString::FromUTF8(ragtime::files::groupedNumber(size)),
                                 (int)(100 * bytes / size));
    else if (size > 0)
        line += ", " + wxString::FromUTF8(ragtime::files::groupedNumber(size)) + " bytes";
    about_->SetLabel(line);
}

void PictureFrame::zoomed(double scale)
{
    zoom_->SetValue(std::max(zoom_->GetMin(), std::min(zoom_->GetMax(), sliderFor(scale))));
    zoomText_->SetLabel(wxString::Format("%d%%", (int)std::lround(scale * 100.0)));
}

void PictureFrame::save()
{
    if (!image_.IsOk()) return;
    wxString name = wxFileName(wxString::FromUTF8(suggestedName_)).GetName() + ".png";
    wxFileDialog pick(this, "Save the picture", "", name, "PNG pictures (*.png)|*.png",
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (pick.ShowModal() != wxID_OK) return;
    wxString path = pick.GetPath();
    if (wxFileName(path).GetExt().empty()) path += ".png";
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
    if (!image_.SaveFile(path, wxBITMAP_TYPE_PNG))
        wxMessageBox("It couldn't be saved there.", "Save the picture", wxOK | wxICON_WARNING, this);
}
