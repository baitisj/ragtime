// The microphone deck's drawn parts: see DeckParts.h. The knob's feel is
// Glissando's Dial (ChaoticaControls.cpp, by Claude and Jeff, ported under
// MIT), its look Ragtime's plain panel.
#include "DeckParts.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>
#include <wx/sizer.h>

#include "Theme.h"

using namespace ragtime;

namespace
{
constexpr double PI = 3.14159265358979323846;
// A knob turns through 270 degrees, as Glissando's dials do.
constexpr double KNOB_SWEEP = 270.0 * PI / 180.0;
// A meter's needle swings 50 degrees either side of upright.
constexpr double METER_HALF_SWEEP = 50.0 * PI / 180.0;

wxColour rgb(unsigned v)
{
    return wxColour((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

wxColour withAlpha(const wxColour& c, int alpha)
{
    return wxColour(c.Red(), c.Green(), c.Blue(), alpha);
}

void glow(wxGraphicsContext& gc, double x, double y, double r, const wxColour& c, int alpha)
{
    wxGraphicsBrush b = gc.CreateRadialGradientBrush(x, y, x, y, r, withAlpha(c, alpha), withAlpha(c, 0));
    gc.SetBrush(b);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.DrawEllipse(x - r, y - r, 2 * r, 2 * r);
}

} // namespace

namespace deckart
{

Colours colours()
{
    Colours c;
    if (themeIsDark())
    {
        c.plate = rgb(0x1f1e1b);
        c.module = rgb(0x2c2a26);
        c.edge = rgb(0x45413a);
        c.ink = rgb(0xe9e2cf);
        c.dimInk = rgb(0xb3ab98);
        c.knob = rgb(0x0f0e0d);
        c.knobCap = rgb(0x24221f);
    }
    else
    {
        c.plate = rgb(0xd6d0c2);
        c.module = rgb(0xe5dfd2);
        c.edge = rgb(0xb8b1a2);
        c.ink = rgb(0x2b2823);
        c.dimInk = rgb(0x4a463e);
        c.knob = rgb(0x1f1d1b);
        c.knobCap = rgb(0x2f2c28);
    }
    c.amber = rgb(0xe8a634);
    c.amberDark = rgb(0x5c4a26);
    c.green = rgb(0x3f8a52);
    c.greenDark = rgb(0x2f4a33);
    c.red = rgb(0xe0392e);
    c.redDark = rgb(0x5d2621);
    c.pointer = rgb(0xefe6cf);
    return c;
}

wxFont captionFont(const wxWindow* w, double points)
{
    wxFont f = w->GetFont();
    f.SetFractionalPointSize(points);
    f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    return f;
}

wxFont readoutFont(const wxWindow* w, double points)
{
    wxFont f(wxFontInfo().Family(wxFONTFAMILY_TELETYPE));
    f.SetFractionalPointSize(points);
    (void)w;
    return f;
}

double captionWidth(wxGraphicsContext& gc, const wxString& text, double spacing)
{
    double total = 0.0;
    for (size_t i = 0; i < text.length(); i++)
    {
        double cw = 0, ch = 0;
        gc.GetTextExtent(wxString(text[i]), &cw, &ch);
        total += cw + (i + 1 < text.length() ? spacing : 0.0);
    }
    return total;
}

void drawCaption(wxGraphicsContext& gc, const wxString& text, double x, double y, double spacing)
{
    double at = x - captionWidth(gc, text, spacing) / 2.0;
    for (size_t i = 0; i < text.length(); i++)
    {
        wxString ch(text[i]);
        double cw = 0, chh = 0;
        gc.GetTextExtent(ch, &cw, &chh);
        gc.DrawText(ch, at, y);
        at += cw + spacing;
    }
}

} // namespace deckart

//-------------------------------------------------------------------------
// DeckModule
//-------------------------------------------------------------------------

DeckModule::DeckModule(wxWindow* parent, const wxString& title) : wxPanel(parent, wxID_ANY), title_(title)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(deckart::colours().module);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->AddSpacer(title_.empty() ? 6 : FromDIP(26));
    content_ = new wxBoxSizer(wxVERTICAL);
    outer->Add(content_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(outer);
    Bind(wxEVT_PAINT, &DeckModule::onPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
        SetBackgroundColour(deckart::colours().module);
        Refresh();
        e.Skip();
    });
}

void DeckModule::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    deckart::Colours c = deckart::colours();
    dc.SetBackground(wxBrush(c.plate));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    wxSize s = GetClientSize();
    gc->SetPen(wxPen(c.edge, 1));
    gc->SetBrush(wxBrush(c.module));
    gc->DrawRoundedRectangle(0.5, 0.5, s.x - 1.0, s.y - 1.0, FromDIP(6));
    if (title_.empty()) return;
    gc->SetFont(deckart::captionFont(this, 8.5), c.ink);
    deckart::drawCaption(*gc, title_, s.x / 2.0, FromDIP(7), FromDIP(22) / 10.0);
    gc->SetPen(wxPen(c.edge, 1));
    gc->StrokeLine(FromDIP(8), FromDIP(22), s.x - FromDIP(8), FromDIP(22));
}

//-------------------------------------------------------------------------
// DeckControl
//-------------------------------------------------------------------------

DeckControl::DeckControl(wxWindow* parent, wxWindowID id, const wxSize& size, long style)
    : wxControl(parent, id, wxDefaultPosition, size, wxBORDER_NONE | style)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(size);
    Bind(wxEVT_PAINT, &DeckControl::onPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
        Refresh();
        e.Skip();
    });
}

void DeckControl::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(deckart::colours().module));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) paint(*gc, GetClientSize());
}

//-------------------------------------------------------------------------
// DeckKnob
//-------------------------------------------------------------------------

DeckKnob::DeckKnob(wxWindow* parent, wxWindowID id, const wxString& caption, const deck::KnobSpec& spec,
                   const wxSize& size)
    : DeckControl(parent, id, size, wxWANTS_CHARS), knob_(spec), caption_(caption)
{
    formatter_ = [](double v, bool) { return wxString::Format("%.1f", v); };
    SetCursor(wxCursor(wxCURSOR_SIZENS));
    wxString tip = _("Drag up or down, or turn the mouse wheel; Shift for fine steps, Control for big ones. "
                     "The arrow keys turn it too, Page Up and Page Down by a big step.");
    if (spec.positions.empty()) tip += _(" Double click to return to the default, where dragging also catches.");
    if (spec.pushable) tip += _(" Click without turning to push it in or pop it out (or Space); a double click "
                                "puts the default back, pushed as it was.");
    SetToolTip(tip);
    Bind(wxEVT_LEFT_DOWN, &DeckKnob::onDown, this);
    Bind(wxEVT_LEFT_UP, &DeckKnob::onUp, this);
    Bind(wxEVT_MOTION, &DeckKnob::onMove, this);
    Bind(wxEVT_MOUSEWHEEL, &DeckKnob::onWheel, this);
    Bind(wxEVT_LEFT_DCLICK, &DeckKnob::onDouble, this);
    Bind(wxEVT_KEY_DOWN, &DeckKnob::onKey, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &DeckKnob::onLostCapture, this);
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        Refresh();
        e.Skip();
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        Refresh();
        e.Skip();
    });
}

void DeckKnob::setValue(double value)
{
    if (knob_.setValue(value)) Refresh();
}

void DeckKnob::setPushed(bool pushed)
{
    if (knob_.setPushed(pushed)) Refresh();
}

void DeckKnob::setFormatter(Formatter f)
{
    formatter_ = std::move(f);
    Refresh();
}

void DeckKnob::setRingWhenOut(bool out)
{
    ringWhenOut_ = out;
    Refresh();
}

void DeckKnob::setNote(const wxString& note)
{
    note_ = note;
    Refresh();
}

void DeckKnob::sendChanged()
{
    Refresh();
    wxCommandEvent e(wxEVT_SLIDER, GetId());
    e.SetEventObject(this);
    e.SetInt((int)std::lround(knob_.value() * 10.0));
    ProcessWindowEvent(e);
}

void DeckKnob::sendPushed()
{
    Refresh();
    wxCommandEvent e(wxEVT_TOGGLEBUTTON, GetId());
    e.SetEventObject(this);
    e.SetInt(knob_.pushed() ? 1 : 0);
    ProcessWindowEvent(e);
}

void DeckKnob::onDown(wxMouseEvent& e)
{
    if (!IsEnabled()) return;
    SetFocus();
    knob_.press(e.GetY());
    if (!HasCapture()) CaptureMouse();
}

void DeckKnob::onUp(wxMouseEvent& e)
{
    if (HasCapture()) ReleaseMouse();
    if (knob_.release(e.GetY())) sendPushed();
    Refresh();
}

void DeckKnob::onMove(wxMouseEvent& e)
{
    if (!knob_.dragging()) return;
    if (knob_.drag(e.GetY(), e.ShiftDown())) sendChanged();
}

void DeckKnob::onWheel(wxMouseEvent& e)
{
    if (!IsEnabled()) return;
    if (knob_.wheel(e.GetWheelRotation(), e.GetWheelDelta(), e.ShiftDown(), e.ControlDown())) sendChanged();
}

void DeckKnob::onDouble(wxMouseEvent&)
{
    if (!IsEnabled()) return;
    bool wasPushed = knob_.pushed();
    double was = knob_.value();
    if (!knob_.doubleClick()) return;
    if (knob_.value() != was) sendChanged();
    if (knob_.pushed() != wasPushed) sendPushed();
}

void DeckKnob::onKey(wxKeyEvent& e)
{
    deck::KnobKey key;
    switch (e.GetKeyCode())
    {
    case WXK_UP:
    case WXK_NUMPAD_UP: key = deck::KnobKey::Up; break;
    case WXK_DOWN:
    case WXK_NUMPAD_DOWN: key = deck::KnobKey::Down; break;
    case WXK_LEFT:
    case WXK_NUMPAD_LEFT: key = deck::KnobKey::Left; break;
    case WXK_RIGHT:
    case WXK_NUMPAD_RIGHT: key = deck::KnobKey::Right; break;
    case WXK_PAGEUP:
    case WXK_NUMPAD_PAGEUP: key = deck::KnobKey::PageUp; break;
    case WXK_PAGEDOWN:
    case WXK_NUMPAD_PAGEDOWN: key = deck::KnobKey::PageDown; break;
    case WXK_HOME: key = deck::KnobKey::Home; break;
    case WXK_END: key = deck::KnobKey::End; break;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        key = deck::KnobKey::Push;
        break;
    default:
        // Tab moves on; Space is Push to talk's while the deck has focus,
        // so the window sees it.
        e.Skip();
        return;
    }
    if (key == deck::KnobKey::Push)
    {
        if (knob_.key(key, false)) sendPushed();
        else e.Skip();
        return;
    }
    if (knob_.key(key, e.ShiftDown())) sendChanged();
}

void DeckKnob::onLostCapture(wxMouseCaptureLostEvent&)
{
    knob_.release(-100000);
    Refresh();
}

void DeckKnob::paint(wxGraphicsContext& gc, const wxSize& size)
{
    deckart::Colours c = deckart::colours();
    const double captionH = FromDIP(12), readoutH = FromDIP(13), noteH = note_.empty() ? 0 : FromDIP(11);
    double radius = std::min(size.x - 8.0, size.y - captionH - readoutH - noteH - 6.0) / 2.0;
    double cx = size.x / 2.0, cy = 3.0 + radius;
    bool enabled = IsEnabled();

    // Ticks around the knob: at each position of a switch, else seven.
    const auto& positions = knob_.spec().positions;
    int ticks = positions.empty() ? 6 : std::max(1, (int)positions.size() - 1);
    double start = -PI / 2.0 - KNOB_SWEEP / 2.0;
    gc.SetPen(wxPen(c.dimInk, 1));
    for (int i = 0; i <= ticks; i++)
    {
        double a = start + KNOB_SWEEP * i / ticks;
        gc.StrokeLine(cx + radius * 0.84 * std::cos(a), cy + radius * 0.84 * std::sin(a),
                      cx + radius * 0.98 * std::cos(a), cy + radius * 0.98 * std::sin(a));
    }

    // The ring: lit amber when in (or out, for Target).
    bool ring = knob_.spec().pushable && (knob_.pushed() != ringWhenOut_);
    double skirt = radius * (knob_.spec().pushable && knob_.pushed() ? 0.66 : 0.70);
    if (ring)
    {
        glow(gc, cx, cy, radius * 0.95, c.amber, enabled ? 90 : 30);
        gc.SetPen(wxPen(enabled ? c.amber : c.amberDark, std::max(2, FromDIP(2))));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawEllipse(cx - radius * 0.77, cy - radius * 0.77, radius * 1.54, radius * 1.54);
    }

    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(c.knob));
    gc.DrawEllipse(cx - skirt, cy - skirt, 2 * skirt, 2 * skirt);
    // Knurls on the skirt.
    gc.SetPen(wxPen(wxColour(255, 255, 255, 28), 1));
    for (int i = 0; i < 24; i++)
    {
        double a = 2.0 * PI * i / 24.0;
        gc.StrokeLine(cx + skirt * 0.80 * std::cos(a), cy + skirt * 0.80 * std::sin(a), cx + skirt * std::cos(a),
                      cy + skirt * std::sin(a));
    }
    double cap = skirt * 0.74;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(gc.CreateLinearGradientBrush(cx, cy - cap, cx, cy + cap, c.knobCap.ChangeLightness(130), c.knobCap));
    gc.DrawEllipse(cx - cap, cy - cap, 2 * cap, 2 * cap);

    // The pointer: a triangle for a switch, else a line.
    double a = start + KNOB_SWEEP * knob_.fraction();
    wxColour pointer = enabled ? c.pointer : c.pointer.ChangeLightness(60);
    if (!positions.empty())
    {
        wxGraphicsPath tri = gc.CreatePath();
        double tip = skirt * 1.0, base = skirt * 0.62, half = 0.22;
        tri.MoveToPoint(cx + tip * std::cos(a), cy + tip * std::sin(a));
        tri.AddLineToPoint(cx + base * std::cos(a - half), cy + base * std::sin(a - half));
        tri.AddLineToPoint(cx + base * std::cos(a + half), cy + base * std::sin(a + half));
        tri.CloseSubpath();
        gc.SetBrush(wxBrush(pointer));
        gc.FillPath(tri);
    }
    else
    {
        gc.SetPen(wxPen(pointer, std::max(2, FromDIP(2)), wxPENSTYLE_SOLID));
        gc.StrokeLine(cx + cap * 0.15 * std::cos(a), cy + cap * 0.15 * std::sin(a), cx + skirt * 0.92 * std::cos(a),
                      cy + skirt * 0.92 * std::sin(a));
    }

    // Focus: a dotted ring, for the keys.
    if (HasFocus())
    {
        wxPen dots(c.ink, 1, wxPENSTYLE_DOT);
        gc.SetPen(dots);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.DrawEllipse(cx - radius, cy - radius, 2 * radius, 2 * radius);
    }

    double y = cy + radius + 2;
    gc.SetFont(deckart::captionFont(this, 7.5), enabled ? c.ink : c.dimInk);
    deckart::drawCaption(gc, caption_, cx, y, dip(1.4));
    y += captionH;
    gc.SetFont(deckart::readoutFont(this, 8.5), enabled ? c.dimInk : c.edge);
    wxString text = formatter_(knob_.value(), knob_.pushed());
    double tw = 0, th = 0;
    gc.GetTextExtent(text, &tw, &th);
    gc.DrawText(text, cx - tw / 2, y);
    if (!note_.empty())
    {
        y += readoutH;
        gc.SetFont(deckart::readoutFont(this, 7.0), c.dimInk);
        gc.GetTextExtent(note_, &tw, &th);
        gc.DrawText(note_, cx - tw / 2, y);
    }
}

//-------------------------------------------------------------------------
// DeckMeter
//-------------------------------------------------------------------------

DeckMeter::DeckMeter(wxWindow* parent, Face face, double floor, double top, std::vector<double> labels,
                     std::vector<Band> bands, const wxSize& size)
    : DeckControl(parent, wxID_ANY, size), face_(face), floor_(floor), top_(top), labels_(std::move(labels)),
      bands_(std::move(bands)), needle_(floor), hold_(NAN)
{
}

void DeckMeter::setNeedle(double dbfs)
{
    if (std::fabs(dbfs - needle_) < 0.05) return;
    needle_ = dbfs;
    Refresh();
}

void DeckMeter::setHold(double dbfs)
{
    if ((std::isnan(dbfs) && std::isnan(hold_)) || std::fabs(dbfs - hold_) < 0.05) return;
    hold_ = dbfs;
    Refresh();
}

double DeckMeter::angleOf(double dbfs) const
{
    // Upright is 0; left is negative.
    return -METER_HALF_SWEEP + 2.0 * METER_HALF_SWEEP * deck::faceFraction(dbfs, floor_, top_);
}

void DeckMeter::paint(wxGraphicsContext& gc, const wxSize& size)
{
    const bool black = face_ == Face::Black;
    const wxColour faceColour = black ? rgb(0x1d1c1a) : rgb(0xf2ead2);
    const wxColour ink = black ? rgb(0xe9e2cf) : rgb(0x2b2823);
    const wxColour green = black ? rgb(0x4fae68) : rgb(0x3f8a52);
    const wxColour red = rgb(0xe0392e);
    const wxColour needle = black ? rgb(0xf4efe2) : rgb(0x1b1a17);

    // The window in the panel.
    gc.SetPen(wxPen(rgb(0x3a3631), std::max(2, FromDIP(2))));
    gc.SetBrush(wxBrush(faceColour));
    gc.DrawRoundedRectangle(1, 1, size.x - 2.0, size.y - 2.0, FromDIP(4));
    gc.Clip(3, 3, size.x - 6.0, size.y - 6.0);

    // The arc's centre sits below the window, as on a real meter.
    double r = std::min(size.x * 0.52, size.y * 0.95);
    double cx = size.x / 2.0, cy = size.y * 0.22 + r;
    auto pt = [&](double angle, double radius, double& x, double& y) {
        x = cx + radius * std::sin(angle);
        y = cy - radius * std::cos(angle);
    };
    auto arc = [&](double from, double to, double radius, const wxColour& colour, double width) {
        wxGraphicsPath p = gc.CreatePath();
        p.AddArc(cx, cy, radius, -PI / 2 + angleOf(from), -PI / 2 + angleOf(to), true);
        gc.SetPen(wxPen(colour, std::max(1.0, width)));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.StrokePath(p);
    };

    // The scale's arc, then its bands (green on target, red where sotto
    // saturates).
    arc(floor_, top_, r * 0.80, ink, 1.0);
    for (const Band& b : bands_) arc(b.low, b.high, r * (b.red ? 0.80 : 0.86), b.red ? red : green, r * (b.red ? 0.07 : 0.09));

    // Ticks and labels.
    gc.SetFont(deckart::readoutFont(this, 6.5), ink);
    for (double l : labels_)
    {
        double a = angleOf(l), x0, y0, x1, y1;
        pt(a, r * 0.80, x0, y0);
        pt(a, r * 0.90, x1, y1);
        gc.SetPen(wxPen(ink, 1));
        gc.StrokeLine(x0, y0, x1, y1);
        wxString text = l < 0 ? wxString::FromUTF8("\xE2\x88\x92") + wxString::Format("%g", -l) : wxString::Format("%g", l);
        double tw = 0, th = 0, tx, ty;
        gc.GetTextExtent(text, &tw, &th);
        pt(a, r * 0.98 + th * 0.4, tx, ty);
        bool redLabel = false;
        for (const Band& b : bands_)
            if (b.red && l >= b.low) redLabel = true;
        gc.SetFont(deckart::readoutFont(this, 6.5), redLabel ? rgb(0xff7a6e) : ink);
        gc.DrawText(text, tx - tw / 2, ty - th / 2);
    }

    // The hold mark, then the needle.
    if (!std::isnan(hold_) && hold_ > floor_)
    {
        double x0, y0, x1, y1;
        pt(angleOf(hold_), r * 0.70, x0, y0);
        pt(angleOf(hold_), r * 0.88, x1, y1);
        gc.SetPen(wxPen(rgb(0xf0a43a), std::max(2, FromDIP(2))));
        gc.StrokeLine(x0, y0, x1, y1);
    }
    double x0, y0, x1, y1;
    pt(angleOf(needle_), r * 0.30, x0, y0);
    pt(angleOf(needle_), r * 0.92, x1, y1);
    gc.SetPen(wxPen(needle, std::max(1.5, dip(1.6))));
    gc.StrokeLine(x0, y0, x1, y1);
    gc.ResetClip();
}

//-------------------------------------------------------------------------
// DeckLamp
//-------------------------------------------------------------------------

DeckLamp::DeckLamp(wxWindow* parent, const wxString& caption, Colour colour, bool captionBelow, const wxSize& size)
    : DeckControl(parent, wxID_ANY,
                  size != wxDefaultSize ? size
                  : caption.empty()     ? parent->FromDIP(wxSize(16, 16))
                  : captionBelow        ? parent->FromDIP(wxSize(56, 32))
                                        : parent->FromDIP(wxSize(80, 18))),
      caption_(caption), colour_(colour), below_(captionBelow)
{
}

void DeckLamp::setLit(bool lit)
{
    if (lit_ == lit) return;
    lit_ = lit;
    Refresh();
}

void DeckLamp::setColour(Colour colour)
{
    if (colour_ == colour) return;
    colour_ = colour;
    Refresh();
}

void DeckLamp::paint(wxGraphicsContext& gc, const wxSize& size)
{
    deckart::Colours c = deckart::colours();
    wxColour on = colour_ == Colour::Amber ? c.amber : colour_ == Colour::Green ? c.green : c.red;
    wxColour off = colour_ == Colour::Amber ? c.amberDark : colour_ == Colour::Green ? c.greenDark : c.redDark;
    double r = FromDIP(6);
    double cx, cy;
    if (caption_.empty())
    {
        cx = size.x / 2.0;
        cy = size.y / 2.0;
    }
    else if (below_)
    {
        cx = size.x / 2.0;
        cy = r + 2;
    }
    else
    {
        cx = r + 2;
        cy = size.y / 2.0;
    }
    if (lit_) glow(gc, cx, cy, r * 2.2, on, 120);
    gc.SetPen(wxPen(lit_ ? on.ChangeLightness(60) : c.ink, std::max(1.0, dip(1.5))));
    gc.SetBrush(wxBrush(lit_ ? on : off));
    gc.DrawEllipse(cx - r, cy - r, 2 * r, 2 * r);
    if (caption_.empty()) return;
    gc.SetFont(deckart::captionFont(this, 7.0), lit_ ? c.ink : c.dimInk);
    double tw = 0, th = 0;
    gc.GetTextExtent(caption_, &tw, &th);
    if (below_)
        deckart::drawCaption(gc, caption_, cx, cy + r + 3, dip(0.8));
    else
        gc.DrawText(caption_, cx + r + FromDIP(6), cy - th / 2);
}

//-------------------------------------------------------------------------
// ReductionBar
//-------------------------------------------------------------------------

ReductionBar::ReductionBar(wxWindow* parent) : DeckControl(parent, wxID_ANY, parent->FromDIP(wxSize(130, 44)))
{
    SetToolTip(_("The leveller's gain reduction. Normal talking sits in the green bracket, 3 to 6 dB."));
}

void ReductionBar::setReduction(double db)
{
    db = std::max(0.0, std::min(deck::REDUCTION_TOP_DB, db));
    if (std::fabs(db - db_) < 0.1) return;
    db_ = db;
    Refresh();
}

void ReductionBar::paint(wxGraphicsContext& gc, const wxSize& size)
{
    deckart::Colours c = deckart::colours();
    const int segments = 8;
    double pad = FromDIP(4);
    double barH = FromDIP(18);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(rgb(0x1f1d1a)));
    gc.DrawRoundedRectangle(0, 0, size.x, barH, FromDIP(3));
    double gap = FromDIP(3);
    double w = (size.x - 2 * pad - gap * (segments - 1)) / segments;
    double per = deck::REDUCTION_TOP_DB / segments;
    for (int i = 0; i < segments; i++)
    {
        bool lit = db_ > i * per + per * 0.25;
        gc.SetBrush(wxBrush(lit ? c.amber : rgb(0x4a3d26)));
        gc.DrawRoundedRectangle(pad + i * (w + gap), pad, w, barH - 2 * pad, 1);
    }
    // The bracket under it, 3 to 6 dB.
    double x0 = pad + (deck::REDUCTION_BAND_LOW_DB / deck::REDUCTION_TOP_DB) * (size.x - 2 * pad);
    double x1 = pad + (deck::REDUCTION_BAND_HIGH_DB / deck::REDUCTION_TOP_DB) * (size.x - 2 * pad);
    gc.SetBrush(wxBrush(c.green));
    gc.DrawRoundedRectangle(x0, barH + FromDIP(2), x1 - x0, FromDIP(3), 1);
    gc.SetFont(deckart::readoutFont(this, 6.8), c.dimInk);
    double tw = 0, th = 0;
    double ty = barH + FromDIP(7);
    gc.DrawText("0", 0, ty);
    wxString top = "20 dB";
    gc.GetTextExtent(top, &tw, &th);
    gc.DrawText(top, size.x - tw, ty);
    gc.SetFont(deckart::readoutFont(this, 6.8), c.green);
    wxString mid = wxString::FromUTF8("target 3\xE2\x80\x93" "6");
    gc.GetTextExtent(mid, &tw, &th);
    gc.DrawText(mid, (size.x - tw) / 2, ty);
}

//-------------------------------------------------------------------------
// PitchScope
//-------------------------------------------------------------------------

namespace
{
// Four seconds of frames across the screen.
constexpr size_t SCOPE_FRAMES = 200;
constexpr double SCOPE_LOW_HZ = 60.0;
constexpr double SCOPE_HIGH_HZ = 420.0;
} // namespace

PitchScope::PitchScope(wxWindow* parent, const wxSize& size) : DeckControl(parent, wxID_ANY, size)
{
    SetToolTip(_("The pitch of the last few seconds, read from sotto's frames as they are coded: "
                 "height is pitch, thickness loudness, a gap silence or hiss."));
}

void PitchScope::add(double pitchHz, double dbfs)
{
    points_.push_back({pitchHz, dbfs});
    while (points_.size() > SCOPE_FRAMES) points_.pop_front();
    Refresh();
}

void PitchScope::clear()
{
    points_.clear();
    Refresh();
}

void PitchScope::paint(wxGraphicsContext& gc, const wxSize& size)
{
    const wxColour screen = rgb(0x14201a), grid = rgb(0x1f3a2c), trace = rgb(0x8cf0a8), label = rgb(0x5fae7a);
    gc.SetPen(wxPen(rgb(0x2b2823), std::max(3, FromDIP(3))));
    gc.SetBrush(wxBrush(screen));
    gc.DrawRoundedRectangle(1.5, 1.5, size.x - 3.0, size.y - 3.0, FromDIP(8));
    double l = FromDIP(8), t = FromDIP(8), w = size.x - 2 * l, h = size.y - 2 * t;
    gc.SetPen(wxPen(grid, 1));
    for (int i = 1; i < 4; i++)
    {
        gc.StrokeLine(l, t + h * i / 4.0, l + w, t + h * i / 4.0);
        gc.StrokeLine(l + w * i / 4.0, t, l + w * i / 4.0, t + h);
    }
    gc.SetFont(deckart::readoutFont(this, 6.5), label);
    gc.DrawText("pitch", l + 2, t);
    double tw = 0, th = 0;
    gc.GetTextExtent("now", &tw, &th);
    gc.DrawText("now", l + w - tw - 2, t + h - th);

    // The trace: one segment a frame, joined while voiced.
    const double lo = std::log(SCOPE_LOW_HZ), hi = std::log(SCOPE_HIGH_HZ);
    auto yOf = [&](double hz) {
        double f = (std::log(std::max(SCOPE_LOW_HZ, std::min(SCOPE_HIGH_HZ, hz))) - lo) / (hi - lo);
        return t + h * (1.0 - f) * 0.9 + h * 0.05;
    };
    double step = w / (double)SCOPE_FRAMES;
    double x0 = l + w - step * points_.size();
    for (size_t i = 1; i < points_.size(); i++)
    {
        const Point& a = points_[i - 1];
        const Point& b = points_[i];
        if (!(a.hz > 0) || !(b.hz > 0)) continue;
        double loud = std::max(0.0, std::min(1.0, (b.dbfs + 50.0) / 40.0));
        gc.SetPen(wxPen(trace, std::max(1.0, dip(1.0 + 3.5 * loud)), wxPENSTYLE_SOLID));
        gc.StrokeLine(x0 + (i - 1) * step, yOf(a.hz), x0 + i * step, yOf(b.hz));
    }
    if (!points_.empty() && points_.back().hz > 0)
    {
        double x = x0 + (points_.size() - 1) * step, y = yOf(points_.back().hz);
        glow(gc, x, y, FromDIP(5), rgb(0xd4ffe0), 200);
    }
}

//-------------------------------------------------------------------------
// AirtimeBar
//-------------------------------------------------------------------------

AirtimeBar::AirtimeBar(wxWindow* parent) : DeckControl(parent, wxID_ANY, parent->FromDIP(wxSize(300, 34)))
{
    SetToolTip(_("Seconds on the air for the clip so far, at the gear it will go in. "
                 "The red mark is the Max airtime setting; past it the bar turns red."));
}

void AirtimeBar::set(const deck::AirBar& bar)
{
    if (std::fabs(bar.airSeconds - bar_.airSeconds) < 0.05 && bar.maxSeconds == bar_.maxSeconds) return;
    bar_ = bar;
    Refresh();
}

void AirtimeBar::paint(wxGraphicsContext& gc, const wxSize& size)
{
    deckart::Colours c = deckart::colours();
    double barH = FromDIP(16);
    double scale = bar_.scaleSeconds();
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(rgb(0x1f1d1a)));
    gc.DrawRoundedRectangle(0, 0, size.x, barH, FromDIP(3));
    double fill = std::min(1.0, bar_.airSeconds / scale) * size.x;
    gc.SetBrush(wxBrush(bar_.over() ? c.red : c.amber));
    if (fill > 0) gc.DrawRoundedRectangle(0, 0, fill, barH, FromDIP(3));
    double mark = std::min(1.0, bar_.maxSeconds / scale) * size.x;
    gc.SetBrush(wxBrush(c.red));
    gc.DrawRectangle(std::max(0.0, mark - 2), 0, 2, barH);

    // The scale under it: 0, quarters, and the mark.
    gc.SetFont(deckart::readoutFont(this, 6.8), c.dimInk);
    double ty = barH + FromDIP(2);
    for (int i = 0; i < 4; i++)
    {
        double s = bar_.maxSeconds * i / 4.0;
        wxString text = i == 0 ? wxString("0 s") : wxString::Format("%g", std::round(s));
        double tw = 0, th = 0;
        gc.GetTextExtent(text, &tw, &th);
        double x = s / scale * size.x - (i == 0 ? 0 : tw / 2);
        gc.DrawText(text, x, ty);
    }
    wxString max = wxString::Format("%g s max", std::round(bar_.maxSeconds));
    double tw = 0, th = 0;
    gc.GetTextExtent(max, &tw, &th);
    gc.SetFont(deckart::readoutFont(this, 6.8), c.red.ChangeLightness(80));
    gc.DrawText(max, std::min(size.x - tw, mark - tw), ty);
}

//-------------------------------------------------------------------------
// TalkButton
//-------------------------------------------------------------------------

TalkButton::TalkButton(wxWindow* parent, const wxSize& size) : DeckControl(parent, wxID_ANY, size)
{
    SetToolTip(_("Hold to record, let go to send (or hold the space bar while the deck has focus). "
                 "Esc while holding throws the clip away."));
    Bind(wxEVT_LEFT_DOWN, &TalkButton::onDown, this);
    Bind(wxEVT_LEFT_DCLICK, &TalkButton::onDown, this);
    Bind(wxEVT_LEFT_UP, &TalkButton::onUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &TalkButton::onLostCapture, this);
}

void TalkButton::setHeld(bool held)
{
    if (held_ == held) return;
    held_ = held;
    if (!held_) mouse_ = false;
    Refresh();
}

void TalkButton::setClock(const wxString& clock)
{
    if (clock_ == clock) return;
    clock_ = clock;
    Refresh();
}

void TalkButton::send(bool down)
{
    wxCommandEvent e(wxEVT_BUTTON, GetId());
    e.SetEventObject(this);
    e.SetInt(down ? 1 : 0);
    ProcessWindowEvent(e);
}

void TalkButton::onDown(wxMouseEvent&)
{
    if (!IsEnabled() || held_) return;
    mouse_ = true;
    if (!HasCapture()) CaptureMouse();
    send(true);
}

void TalkButton::onUp(wxMouseEvent&)
{
    if (HasCapture()) ReleaseMouse();
    if (!mouse_) return;
    mouse_ = false;
    send(false);
}

void TalkButton::onLostCapture(wxMouseCaptureLostEvent&)
{
    if (!mouse_) return;
    mouse_ = false;
    send(false);
}

void TalkButton::paint(wxGraphicsContext& gc, const wxSize& size)
{
    deckart::Colours c = deckart::colours();
    double r = std::min(size.x, size.y) / 2.0 - FromDIP(8);
    double cx = size.x / 2.0, cy = size.y / 2.0;
    bool enabled = IsEnabled();

    // The bezel, then the button: dark red at rest, lit and sunk while held.
    gc.SetPen(wxPen(c.edge, 1));
    gc.SetBrush(wxBrush(c.module.ChangeLightness(92)));
    gc.DrawEllipse(cx - r - FromDIP(7), cy - r - FromDIP(7), 2 * (r + FromDIP(7)), 2 * (r + FromDIP(7)));
    wxColour face = !enabled ? rgb(0x7a5a55) : held_ ? rgb(0xc0352a) : rgb(0x8e2b22);
    if (held_) glow(gc, cx, cy, r + FromDIP(8), c.red, 110);
    gc.SetPen(wxPen(rgb(0x2b2823), std::max(3, FromDIP(4))));
    gc.SetBrush(held_ ? gc.CreateRadialGradientBrush(cx, cy + r * 0.3, cx, cy, r, face, face.ChangeLightness(70))
                      : gc.CreateRadialGradientBrush(cx - r * 0.3, cy - r * 0.35, cx, cy, r, face.ChangeLightness(125), face));
    gc.DrawEllipse(cx - r, cy - r, 2 * r, 2 * r);

    wxColour ink = rgb(0xfbefd6);
    gc.SetFont(deckart::captionFont(this, 9.5), ink);
    double tw = 0, th = 0;
    gc.GetTextExtent("PUSH", &tw, &th);
    double y = cy - th * (clock_.empty() ? 1.0 : 1.55);
    deckart::drawCaption(gc, "PUSH", cx, y, dip(1.4));
    deckart::drawCaption(gc, "TO TALK", cx, y + th, dip(1.4));
    if (!clock_.empty())
    {
        gc.SetFont(deckart::readoutFont(this, 12.0), ink);
        gc.GetTextExtent(clock_, &tw, &th);
        gc.DrawText(clock_, cx - tw / 2, y + 2.1 * th - th * 0.2);
    }
}
