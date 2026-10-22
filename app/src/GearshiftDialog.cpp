#include "GearshiftDialog.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include "Thresholds.h"

using namespace ragtime::gearbox;

namespace
{

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

wxColour fromRgb(std::uint32_t rgb)
{
    return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// a toward b by t, so greys follow the system's light or dark theme.
wxColour blend(const wxColour& a, const wxColour& b, double t)
{
    auto mix = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(std::lround(x + (y - x) * t));
    };
    return wxColour(mix(a.Red(), b.Red()), mix(a.Green(), b.Green()), mix(a.Blue(), b.Blue()));
}

const wxColour ACCENT(0x1f, 0x5f, 0xbf);      // the current gear, as the mock-ups
const wxColour SUGGESTION(0x1e, 0x6b, 0x34);  // Auto's pick while Auto is off
const wxColour TOO_HARD(0xc0, 0x26, 0x1a);    // a gear the path can't carry

constexpr double PI = 3.14159265358979323846;
constexpr double MIN_BPS = 20.0;
constexpr double MAX_BPS = 10000.0;
constexpr double DOT_RADIUS = 4.5;
constexpr double KNOB_RADIUS = 9.5;       // half a gear knob's height
constexpr double KNOB_HALF_WIDTH = 19.0;  // wide enough for a short name, "E2C"

// Why there are no gears to draw, or empty when there are: no thresholds
// found at all, or thresholds with no widths and speeds yet (data2g-host's
// own file has none; its MODES list gives them).
wxString whyNoGears(const Gearshift& shift)
{
    if (shift.table().empty())
    {
        return "No thresholds found. Ragtime looks for Data2G's own\n"
               "data2g/codes_data/mode_thresholds.json in a Data2G checkout\n"
               "($DATA2G_HOME, ~/Data2G, ~/src/Data2G and the like).\n"
               "Load it with Load thresholds... below.";
    }
    for (const Submode& mode : shift.usable().modes())
    {
        if (mode.hasFacts())
            return wxString();
    }
    return wxString::Format("Thresholds loaded for %d modes. Their widths and speeds\n"
                            "come from data2g-host: the gears appear once it is connected.",
                            static_cast<int>(shift.table().modes().size()));
}

void setPen(wxGraphicsContext& gc, const wxColour& colour, double width,
            wxPenStyle style = wxPENSTYLE_SOLID)
{
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width).Style(style)));
}

} // namespace

// The Submode Map: speed across on a log scale, the S/N each mode needs down
// the side (harder further down), one dot per mode coloured by modulation,
// the best modes on a thin dashed line and the gears as knobs bearing their
// short names on a heavy one. Clicking a knob picks that gear.
class SubmodeMap : public wxPanel
{
public:
    SubmodeMap(wxWindow* parent, const Gearshift& shift, std::function<void(int)> onPickGear)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(640, 430))
        , shift_(shift)
        , onPickGear_(std::move(onPickGear))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(560, 380));
        Bind(wxEVT_PAINT, &SubmodeMap::onPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            Refresh();
            e.Skip();
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            mouse_ = e.GetPosition();
            hovering_ = true;
            Refresh();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            hovering_ = false;
            Refresh();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            const int gear = gearAt(e.GetPosition());
            if (gear > 0 && onPickGear_)
                onPickGear_(gear);
        });
    }

private:
    struct Plot
    {
        double left, top, right, bottom;
        double snrTop, snrBottom;  // dB at the top and bottom edges
    };

    Plot plot() const
    {
        const wxSize size = GetClientSize();
        Plot p {58.0, 34.0, size.x - 16.0, size.y - 42.0, -15.0, 30.0};
        // One S/N range for every path, so the dots move when the path
        // changes but the scale doesn't.
        double lo = 1e9, hi = -1e9;
        for (const Submode& mode : shift_.usable().modes())
        {
            for (Path path : ALL_PATHS)
            {
                if (std::optional<double> dB = mode.thresholdOn(path))
                {
                    lo = std::min(lo, *dB);
                    hi = std::max(hi, *dB);
                }
            }
        }
        if (lo <= hi)
        {
            p.snrTop = std::floor((lo - 1.0) / 5.0) * 5.0;
            p.snrBottom = std::ceil((hi + 1.0) / 5.0) * 5.0;
        }
        return p;
    }

    static double xFor(const Plot& p, double bps)
    {
        const double t = (std::log10(bps) - std::log10(MIN_BPS))
                         / (std::log10(MAX_BPS) - std::log10(MIN_BPS));
        return p.left + t * (p.right - p.left);
    }

    static double yFor(const Plot& p, double dB)
    {
        return p.top + (dB - p.snrTop) / (p.snrBottom - p.snrTop) * (p.bottom - p.top);
    }

    std::vector<std::pair<const Submode*, wxPoint2DDouble>> dots(const Plot& p) const
    {
        std::vector<std::pair<const Submode*, wxPoint2DDouble>> out;
        for (const Submode& mode : shift_.usable().modes())
        {
            const std::optional<double> dB = mode.thresholdOn(shift_.path());
            if (mode.hasFacts() && dB)
                out.emplace_back(&mode, wxPoint2DDouble(xFor(p, mode.bitsPerSecond), yFor(p, *dB)));
        }
        return out;
    }

    int gearAt(const wxPoint& at) const
    {
        const Plot p = plot();
        for (const Gear& gear : shift_.ladder())
        {
            const double dx = at.x - xFor(p, gear.bitsPerSecond);
            const double dy = at.y - yFor(p, gear.threshold);
            if (std::fabs(dx) <= KNOB_HALF_WIDTH + 3 && std::fabs(dy) <= KNOB_RADIUS + 3)
                return gear.number;
        }
        return 0;
    }

    void onPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxColour back = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        const wxColour ink = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        const wxColour faint = blend(back, ink, 0.10);
        const wxColour soft = blend(back, ink, 0.55);
        dc.SetBackground(wxBrush(back));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc)
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const wxFont font = GetFont();
        const wxFont small = font.Smaller();
        const Plot p = plot();

        const wxString empty = whyNoGears(shift_);
        if (!empty.empty())
        {
            gc->SetFont(font, soft);
            const wxString& text = empty;
            double w = 0, h = 0;
            gc->GetTextExtent(text, &w, &h);
            gc->DrawText(text, (GetClientSize().x - w) / 2, (GetClientSize().y - h) / 2);
            return;
        }

        // Grid and axes.
        gc->SetFont(small, soft);
        setPen(*gc, faint, 1);
        const double ticks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
        const char* tickLabels[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k"};
        for (int i = 0; i < 9; i++)
        {
            const double x = xFor(p, ticks[i]);
            gc->StrokeLine(x, p.top, x, p.bottom);
            double w = 0, h = 0;
            gc->GetTextExtent(tickLabels[i], &w, &h);
            gc->DrawText(tickLabels[i], x - w / 2, p.bottom + 4);
        }
        for (double dB = p.snrTop; dB <= p.snrBottom + 0.01; dB += 5.0)
        {
            const double y = yFor(p, dB);
            gc->StrokeLine(p.left, y, p.right, y);
            const wxString label = dB > 0 ? wxString::Format("+%.0f", dB)
                                   : dB < 0 ? wxString::FromUTF8("\xE2\x88\x92")
                                                  + wxString::Format("%.0f", -dB)
                                            : wxString("0");
            double w = 0, h = 0;
            gc->GetTextExtent(label, &w, &h);
            gc->DrawText(label, p.left - w - 6, y - h / 2);
        }
        {
            const wxString across = "bit/s";
            double w = 0, h = 0;
            gc->GetTextExtent(across, &w, &h);
            gc->DrawText(across, (p.left + p.right - w) / 2, p.bottom + 20);
            const wxString down = "S/N needed, dB in 2500 Hz (harder further down)";
            gc->GetTextExtent(down, &w, &h);
            gc->DrawText(down, 4, (p.top + p.bottom + w) / 2, PI / 2);
        }

        // Auto: the path's S/N as a dashed line, and everything needing more
        // than it less the margin shaded.
        const std::optional<double> snr = shift_.pathSnr();
        if (snr)
        {
            const double limit = std::max(p.top, std::min(p.bottom, yFor(p, *snr - AUTO_MARGIN_DB)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(soft.Red(), soft.Green(), soft.Blue(), 48)));
            gc->DrawRectangle(p.left, limit, p.right - p.left, p.bottom - limit);
            const double line = yFor(p, *snr);
            if (line >= p.top && line <= p.bottom)
            {
                setPen(*gc, ink, 1, wxPENSTYLE_SHORT_DASH);
                gc->StrokeLine(p.left, line, p.right, line);
                gc->SetFont(small, ink);
                const wxString label = "path S/N " + utf8(formatDb(*snr));
                double w = 0, h = 0;
                gc->GetTextExtent(label, &w, &h);
                gc->DrawText(label, p.right - w - 4, line - h - 1);
            }
            gc->SetFont(small, soft);
            const wxString margin = wxString::Format("needs more than the path less %.0f dB",
                                                     AUTO_MARGIN_DB);
            double w = 0, h = 0;
            gc->GetTextExtent(margin, &w, &h);
            if (limit + h + 2 < p.bottom)
                gc->DrawText(margin, p.left + 6, p.bottom - h - 4);
        }

        // Every other best mode on a thin dashed line.
        const int cap = widthCapHz(shift_.effectiveWidth());
        const std::vector<const Submode*> best = bestModes(shift_.usable(), shift_.path(), cap);
        if (best.size() > 1)
        {
            std::vector<wxPoint2DDouble> points;
            for (const Submode* mode : best)
                points.emplace_back(xFor(p, mode->bitsPerSecond),
                                    yFor(p, *mode->thresholdOn(shift_.path())));
            setPen(*gc, soft, 1, wxPENSTYLE_SHORT_DASH);
            gc->StrokeLines(points.size(), points.data());
        }

        // The dots; modes wider than the width cap are hollow grey circles.
        const auto all = dots(p);
        for (const auto& [mode, at] : all)
        {
            const double r = DOT_RADIUS;
            if (mode->widthHz > cap)
            {
                setPen(*gc, soft, 1.5);
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
            }
            else
            {
                setPen(*gc, back, 1);
                gc->SetBrush(wxBrush(fromRgb(modulationColour(mode->modulation))));
            }
            gc->DrawEllipse(at.m_x - r, at.m_y - r, 2 * r, 2 * r);
        }

        // The gears: knobs bearing their short names, joined by a heavy line.
        const std::vector<Gear>& ladder = shift_.ladder();
        const std::optional<Gear> current = shift_.current();
        const std::optional<Gear> suggestion = shift_.suggestion();
        if (ladder.size() > 1)
        {
            std::vector<wxPoint2DDouble> points;
            for (const Gear& gear : ladder)
                points.emplace_back(xFor(p, gear.bitsPerSecond), yFor(p, gear.threshold));
            setPen(*gc, ink, 3);
            gc->StrokeLines(points.size(), points.data());
        }
        const wxFont knobFont = small.Bold();
        for (const Gear& gear : ladder)
        {
            const double x = xFor(p, gear.bitsPerSecond);
            const double y = yFor(p, gear.threshold);
            const bool isCurrent = current && current->number == gear.number;
            const bool tooHard = snr && dbShort(gear.threshold, *snr) > 0;
            const wxColour& line = isCurrent ? ACCENT : tooHard ? TOO_HARD : ink;
            // The gear number only for a mode with no short name.
            const wxString label = gear.name.empty() ? wxString::Format("%d", gear.number)
                                                     : utf8(gear.name);
            gc->SetFont(knobFont, isCurrent ? *wxWHITE : line);
            double w = 0, h = 0;
            gc->GetTextExtent(label, &w, &h);
            const double r = KNOB_RADIUS;
            const double half = std::max(KNOB_HALF_WIDTH, w / 2 + 5);
            if (!shift_.isAuto() && suggestion && suggestion->number == gear.number)
            {
                setPen(*gc, SUGGESTION, 2.5);
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRoundedRectangle(x - half - 4, y - r - 4, 2 * (half + 4), 2 * (r + 4), r + 4);
            }
            setPen(*gc, line, 2);
            gc->SetBrush(wxBrush(isCurrent ? ACCENT : back));
            gc->DrawRoundedRectangle(x - half, y - r, 2 * half, 2 * r, r);
            gc->DrawText(label, x - w / 2, y - h / 2);
        }

        // The legend.
        {
            gc->SetFont(small, ink);
            double x = p.left;
            const double y = 10;
            const Modulation kinds[] = {Modulation::Fsk,    Modulation::Qpsk,   Modulation::Qam16,
                                        Modulation::Qam64,  Modulation::Qam256, Modulation::Polar};
            for (Modulation kind : kinds)
            {
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(fromRgb(modulationColour(kind))));
                gc->DrawEllipse(x, y + 2, 9, 9);
                const wxString label = modulationLabel(kind);
                double w = 0, h = 0;
                gc->GetTextExtent(label, &w, &h);
                gc->DrawText(label, x + 13, y + 6.5 - h / 2);
                x += 13 + w + 14;
            }
            setPen(*gc, soft, 1.5);
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawEllipse(x, y + 2, 9, 9);
            const wxString wider = wxString::FromUTF8("wider than ") + widthLabel(shift_.effectiveWidth());
            double w = 0, h = 0;
            gc->GetTextExtent(wider, &w, &h);
            gc->DrawText(wider, x + 13, y + 6.5 - h / 2);
        }

        // Hovering a dot shows its name, width, speed and threshold.
        if (hovering_)
        {
            const Submode* hovered = nullptr;
            double nearest = 81.0;  // 9 px, squared
            for (const auto& [mode, at] : all)
            {
                const double d = (at.m_x - mouse_.x) * (at.m_x - mouse_.x)
                                 + (at.m_y - mouse_.y) * (at.m_y - mouse_.y);
                if (d < nearest)
                {
                    nearest = d;
                    hovered = mode;
                }
            }
            if (hovered)
            {
                const wxString text
                    = utf8(shift_.shortName(hovered->name)) + "   " + utf8(hovered->name) + "\n"
                      + wxString::Format("%d Hz", hovered->widthHz) + wxString::FromUTF8(" \xC2\xB7 ")
                      + modulationLabel(hovered->modulation) + "\n" + utf8(formatSpeed(hovered->bitsPerSecond))
                      + "\n" + utf8(formatDb(*hovered->thresholdOn(shift_.path()))) + " on a "
                      + pathLabel(shift_.path()) + " path";
                gc->SetFont(small, ink);
                double w = 0, h = 0;
                gc->GetTextExtent(text, &w, &h);
                double bx = mouse_.x + 14, by = mouse_.y + 14;
                if (bx + w + 12 > p.right)
                    bx = mouse_.x - w - 26;
                if (by + h + 10 > p.bottom)
                    by = mouse_.y - h - 24;
                setPen(*gc, soft, 1);
                gc->SetBrush(wxBrush(back));
                gc->DrawRoundedRectangle(bx, by, w + 12, h + 10, 4);
                gc->DrawText(text, bx + 6, by + 5);
            }
        }
    }

    const Gearshift& shift_;
    std::function<void(int)> onPickGear_;
    wxPoint mouse_;
    bool hovering_ = false;
};

GearshiftDialog::GearshiftDialog(wxWindow* parent, Gearshift& shift, std::function<void()> onChange)
    : wxDialog(parent, wxID_ANY, "Gearshift", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , shift_(shift)
    , onChange_(std::move(onChange))
{
    auto* top = new wxBoxSizer(wxVERTICAL);

    // Width chips, narrowest to widest, then Path chips, then Auto.
    auto* chips = new wxBoxSizer(wxHORIZONTAL);
    chips->Add(new wxStaticText(this, wxID_ANY, "Width"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    for (std::size_t i = 0; i < ALL_WIDTHS.size(); i++)
    {
        const Width width = ALL_WIDTHS[i];
        auto* chip = new wxToggleButton(this, wxID_ANY, utf8(widthLabel(width)),
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        chip->Bind(wxEVT_TOGGLEBUTTON, [this, width](wxCommandEvent&) {
            shift_.setWidth(width);
            changed();
        });
        widthChips_[i] = chip;
        chips->Add(chip, 0, wxRIGHT, 2);
    }
    chips->AddSpacer(18);
    chips->Add(new wxStaticText(this, wxID_ANY, "Path"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    for (std::size_t i = 0; i < ALL_PATHS.size(); i++)
    {
        const Path path = ALL_PATHS[i];
        auto* chip = new wxToggleButton(this, wxID_ANY, pathLabel(path), wxDefaultPosition,
                                        wxDefaultSize, wxBU_EXACTFIT);
        chip->SetToolTip(path == Path::Calm
                             ? "No fading (AWGN)"
                             : wxString("ITU-R F.1487 ") + pathLabel(path) + " fading channel");
        chip->Bind(wxEVT_TOGGLEBUTTON, [this, path](wxCommandEvent&) {
            shift_.setPath(path);
            changed();
        });
        pathChips_[i] = chip;
        chips->Add(chip, 0, wxRIGHT, 2);
    }
    chips->AddSpacer(18);
    auto_ = new wxCheckBox(this, wxID_ANY,
                           wxString::Format("Auto (%.0f dB margin)", AUTO_MARGIN_DB));
    auto_->SetToolTip("Take the fastest gear that needs no more than the path's S/N, "
                      "less the margin");
    auto_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        shift_.setAuto(e.IsChecked());
        changed();
    });
    chips->Add(auto_, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(chips, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    widthNote_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    widthNote_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    widthNote_->SetFont(widthNote_->GetFont().Smaller());
    top->Add(widthNote_, 0, wxLEFT | wxTOP, 10);

    // The map, and the gear list beside it.
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    map_ = new SubmodeMap(this, shift_, [this](int gear) {
        if (shift_.selectGear(gear))
            changed();
    });
    body->Add(map_, 1, wxEXPAND | wxRIGHT, 10);

    auto* side = new wxBoxSizer(wxVERTICAL);
    inGear_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    inGear_->SetFont(inGear_->GetFont().Bold());
    side->Add(inGear_, 0, wxBOTTOM, 4);
    snr_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    side->Add(snr_, 0, wxBOTTOM, 8);
    gears_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(434, -1),
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    gears_->InsertColumn(0, "Gear", wxLIST_FORMAT_LEFT, 56);
    gears_->InsertColumn(1, "Data2G mode", wxLIST_FORMAT_LEFT, 132);
    gears_->InsertColumn(2, "Speed", wxLIST_FORMAT_RIGHT, 84);
    gears_->InsertColumn(3, "S/N", wxLIST_FORMAT_RIGHT, 76);
    gears_->InsertColumn(4, "1 KB", wxLIST_FORMAT_RIGHT, 80);
    gears_->SetToolTip("Gears in red need more S/N than the path has, less Auto's 3 dB margin");
    gears_->Bind(wxEVT_LIST_ITEM_SELECTED, &GearshiftDialog::onGearSelected, this);
    side->Add(gears_, 1, wxEXPAND);
    body->Add(side, 0, wxEXPAND);
    top->Add(body, 1, wxEXPAND | wxALL, 10);

    // Where the thresholds came from.
    auto* foot = new wxBoxSizer(wxHORIZONTAL);
    source_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_MIDDLE);
    foot->Add(source_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    auto* load = new wxButton(this, wxID_ANY, "Load thresholds...");
    load->SetToolTip("Data2G's data2g/codes_data/mode_thresholds.json, or the Submode Map's file");
    load->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { loadFile(); });
    foot->Add(load, 0, wxRIGHT, 6);
    auto* close = new wxButton(this, wxID_CLOSE);
    // Close, Escape and the title bar's X all end the dialog through the
    // escape id. Binding the button to Close() as well would send the same
    // press round again, and the dialog would never end.
    SetEscapeId(wxID_CLOSE);
    foot->Add(close, 0);
    top->Add(foot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    SetSizerAndFit(top);
    // Fits Jeff's 1366x768 screen with room for the panel and title bar.
    SetSize(wxSize(1144, 610));
    SetMinSize(wxSize(1004, 520));
    refresh();
    CentreOnParent();
}

void GearshiftDialog::changed()
{
    refresh();
    if (onChange_)
        onChange_();
}

void GearshiftDialog::refresh()
{
    refreshing_ = true;
    for (std::size_t i = 0; i < ALL_WIDTHS.size(); i++)
    {
        // The 500 Hz switch greys the wider widths; the chip lit is the
        // width the gears come from.
        widthChips_[i]->Enable(!shift_.narrowOnly() || widthCapHz(ALL_WIDTHS[i]) <= widthCapHz(Width::Hz500));
        widthChips_[i]->SetValue(ALL_WIDTHS[i] == shift_.effectiveWidth());
    }
    widthNote_->SetLabel(shift_.narrowOnly()
                             ? "The 500 Hz switch is on: data2g-host uses only 500 Hz modes, so the gears do too."
                             : "Width picks the modes the gears come from; data2g-host may still use any "
                               "width. The 500 Hz switch on the connection bar limits it.");
    for (std::size_t i = 0; i < ALL_PATHS.size(); i++)
        pathChips_[i]->SetValue(ALL_PATHS[i] == shift_.path());
    auto_->SetValue(shift_.isAuto());

    const std::optional<Gear> current = shift_.current();
    const std::optional<double> snr = shift_.pathSnr();
    if (current)
        inGear_->SetLabel(utf8(gearLabel(*current)) + (current->name.empty() ? "" : ", " + utf8(current->mode))
                          + (shift_.isAuto() && snr ? " (Auto)" : ""));
    else
        inGear_->SetLabel("No gears");
    if (snr)
    {
        const int bursts = static_cast<int>(shift_.snrBursts());
        snr_->SetLabel("Path S/N " + utf8(formatDb(*snr))
                       + (bursts == 1 ? wxString(", from the last burst")
                                      : wxString::Format(", the mean of the last %d bursts",
                                                         bursts)));
    }
    else
    {
        snr_->SetLabel(shift_.isAuto() ? "No S/N reports yet: Auto holds the chosen gear"
                                       : "No S/N reports yet");
    }

    gears_->DeleteAllItems();
    const std::vector<Gear>& ladder = shift_.ladder();
    for (std::size_t i = 0; i < ladder.size(); i++)
    {
        const Gear& gear = ladder[i];
        const long row = gears_->InsertItem(static_cast<long>(i), utf8(gear.name));
        gears_->SetItem(row, 1, utf8(gear.mode));
        gears_->SetItem(row, 2, utf8(formatSpeed(gear.bitsPerSecond)));
        gears_->SetItem(row, 3, utf8(formatDb(gear.threshold)));
        gears_->SetItem(row, 4, utf8(formatDuration(secondsPerKilobyte(gear.bitsPerSecond))));
        if (snr && dbShort(gear.threshold, *snr) > 0)
            gears_->SetItemTextColour(row, TOO_HARD);
        if (current && current->number == gear.number)
        {
            gears_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            gears_->EnsureVisible(row);
        }
    }

    const wxArrayString sources = loadedThresholdSources();
    const int count = static_cast<int>(shift_.table().modes().size());
    if (shift_.table().empty())
        source_->SetLabel("No thresholds found");
    else if (sources.empty())
        source_->SetLabel(wxString::Format("Thresholds for %d modes", count));
    else
        source_->SetLabel(wxString::Format("Thresholds from %s (%d modes)",
                                           wxJoin(sources, ';', '\0'), count));
    source_->SetToolTip(sources.empty()
                            ? "Looked in " + wxJoin(data2gSearchFolders(), ',', '\0')
                            : wxJoin(sources, '\n', '\0'));

    map_->Refresh();
    refreshing_ = false;
}

void GearshiftDialog::onGearSelected(wxListEvent& event)
{
    if (refreshing_)
        return;
    if (shift_.selectGear(static_cast<int>(event.GetIndex()) + 1))
        CallAfter([this]() { changed(); });
}

void GearshiftDialog::loadFile()
{
    wxFileDialog picker(this, "Load Data2G thresholds", wxEmptyString, "mode_thresholds.json",
                        "JSON files (*.json)|*.json|All files|*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (picker.ShowModal() != wxID_OK)
        return;
    SubmodeTable check;
    wxString error;
    if (!loadThresholds(picker.GetPath(), check, error))
    {
        wxMessageBox(error, "Thresholds not loaded", wxOK | wxICON_WARNING, this);
        return;
    }
    // The picked file goes on top of a Data2G checkout's, if one was found.
    rememberThresholdsPath(picker.GetPath());
    wxArrayString sources;
    wxString errors;
    shift_.setTable(loadAllThresholds(sources, errors));
    changed();
}
