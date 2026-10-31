#ifndef RAGTIME_DECK_PARTS_H
#define RAGTIME_DECK_PARTS_H

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <wx/control.h>
#include <wx/panel.h>

#include "ragtime/deck/Knob.h"
#include "ragtime/deck/Meters.h"

class wxGraphicsContext;

// The microphone deck's drawn parts (docs/STAGE1.md, "The microphone
// deck"). Ragtime keeps the system theme and standard widgets; the deck is
// one of the drawn exceptions, like the gearshift and the Globus: a rack
// front panel in the theme's colours, light or dark, with standard
// dropdowns, buttons and boxes for everything that is a choice rather than
// an instrument. No Chaotica art.

namespace deckart
{

// The panel's colours, light or dark as the theme is.
struct Colours
{
    wxColour plate;         // the rack behind the modules
    wxColour module;        // a module's face
    wxColour edge;          // a module's engraved edge
    wxColour ink;           // engraving
    wxColour dimInk;        // secondary engraving
    wxColour amber;         // lit lamps and rings
    wxColour amberDark;     // an amber lamp out
    wxColour green;         // on target
    wxColour greenDark;     // a green lamp out
    wxColour red;           // off target, recording
    wxColour redDark;       // a red lamp out
    wxColour knob;          // a knob's skirt
    wxColour knobCap;       // its cap
    wxColour pointer;       // its pointer
};
Colours colours();

// The engraved captions ("ACTIVE LEVEL") and readouts ("−27 dBFS").
wxFont captionFont(const wxWindow* w, double points = 7.5);
wxFont readoutFont(const wxWindow* w, double points = 8.0);
// A caption with its letters spaced, centred on x.
void drawCaption(wxGraphicsContext& gc, const wxString& text, double x, double y, double spacing);
double captionWidth(wxGraphicsContext& gc, const wxString& text, double spacing);

} // namespace deckart

// A module of the panel: a raised face with an engraved title over a rule
// ("1 · INPUT"). Lay its parts out in content().
class DeckModule : public wxPanel
{
public:
    DeckModule(wxWindow* parent, const wxString& title);
    wxSizer* content() const { return content_; }

private:
    void onPaint(wxPaintEvent& event);
    wxString title_;
    wxSizer* content_;
};

// Common plumbing: double buffered, drawn on the module's face.
class DeckControl : public wxControl
{
public:
    DeckControl(wxWindow* parent, wxWindowID id, const wxSize& size, long style = 0);
    bool AcceptsFocus() const override { return false; }

protected:
    virtual void paint(wxGraphicsContext& gc, const wxSize& size) = 0;
    // Device-independent pixels, fractions kept.
    double dip(double v) const { return FromDIP(1000) / 1000.0 * v; }

private:
    void onPaint(wxPaintEvent& event);
};

// A knob that feels like Glissando's Dial (deck::Knob has the feel): a
// caption over it, the readout under it. It takes focus, for the arrow
// keys. Sends wxEVT_SLIDER when turned and wxEVT_TOGGLEBUTTON (GetInt() 1
// for in) when pushed or popped.
class DeckKnob : public DeckControl
{
public:
    using Formatter = std::function<wxString(double value, bool pushed)>;

    DeckKnob(wxWindow* parent, wxWindowID id, const wxString& caption, const ragtime::deck::KnobSpec& spec,
             const wxSize& size = wxSize(76, 92));

    double value() const { return knob_.value(); }
    bool pushed() const { return knob_.pushed(); }
    void setValue(double value);
    void setPushed(bool pushed);
    void setFormatter(Formatter f);
    // The ring lights when the knob is pushed in (High-pass, Noise), or
    // when it is popped out (Target: peak mode).
    void setRingWhenOut(bool out);
    // A second line under the readout: "push in/out".
    void setNote(const wxString& note);

    bool AcceptsFocus() const override { return IsEnabled(); }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled(); }

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    void sendChanged();
    void sendPushed();
    void onDown(wxMouseEvent& e);
    void onUp(wxMouseEvent& e);
    void onMove(wxMouseEvent& e);
    void onWheel(wxMouseEvent& e);
    void onDouble(wxMouseEvent& e);
    void onKey(wxKeyEvent& e);
    void onLostCapture(wxMouseCaptureLostEvent& e);

    ragtime::deck::Knob knob_;
    wxString caption_;
    wxString note_;
    Formatter formatter_;
    bool ringWhenOut_ = false;
};

// A meter face: VU-style cream (Active level) or broadcast-style black
// (Loudest 10 ms), with its bands, needle and hold mark.
class DeckMeter : public DeckControl
{
public:
    enum class Face
    {
        Cream,
        Black,
    };
    struct Band
    {
        double low, high;   // dBFS
        bool red;           // red zone, else green
    };

    DeckMeter(wxWindow* parent, Face face, double floor, double top, std::vector<double> labels,
              std::vector<Band> bands, const wxSize& size = wxSize(92, 58));

    void setNeedle(double dbfs);
    // NAN for none.
    void setHold(double dbfs);

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    double angleOf(double dbfs) const;
    Face face_;
    double floor_, top_;
    std::vector<double> labels_;
    std::vector<Band> bands_;
    double needle_;
    double hold_;
};

// A round lamp with an optional caption beside or under it.
class DeckLamp : public DeckControl
{
public:
    enum class Colour
    {
        Amber,
        Green,
        Red,
    };
    DeckLamp(wxWindow* parent, const wxString& caption, Colour colour, bool captionBelow,
             const wxSize& size = wxDefaultSize);
    void setLit(bool lit);
    void setColour(Colour colour);
    bool lit() const { return lit_; }

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    wxString caption_;
    Colour colour_;
    bool below_;
    bool lit_ = false;
};

// The leveller's gain reduction, 0 to 20 dB in eight segments, with a
// green bracket at 3 to 6 dB.
class ReductionBar : public DeckControl
{
public:
    explicit ReductionBar(wxWindow* parent);
    void setReduction(double db);

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    double db_ = 0.0;
};

// sotto's pitch trace, drawn on a green phosphor screen from the frames'
// pitch and energy bits as they are coded: the last few seconds, now at
// the right.
class PitchScope : public DeckControl
{
public:
    explicit PitchScope(wxWindow* parent, const wxSize& size = wxSize(184, 104));
    // One frame: its pitch in Hz (0 for unvoiced) and level in dBFS
    // (sotto's silence for silence).
    void add(double pitchHz, double dbfs);
    void clear();

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    struct Point
    {
        double hz, dbfs;
    };
    std::deque<Point> points_;
};

// The airtime bar across the panel's foot.
class AirtimeBar : public DeckControl
{
public:
    explicit AirtimeBar(wxWindow* parent);
    void set(const ragtime::deck::AirBar& bar);

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    ragtime::deck::AirBar bar_;
};

// The big round Push to talk button. Hold it (mouse, or the space bar
// while the deck has focus) to record. It sends wxEVT_BUTTON with GetInt()
// 1 when pressed and 0 when let go.
class TalkButton : public DeckControl
{
public:
    explicit TalkButton(wxWindow* parent, const wxSize& size = wxSize(104, 104));
    void setHeld(bool held);
    bool held() const { return held_; }
    // The clock while recording: "0:14".
    void setClock(const wxString& clock);

protected:
    void paint(wxGraphicsContext& gc, const wxSize& size) override;

private:
    void send(bool down);
    void onDown(wxMouseEvent& e);
    void onUp(wxMouseEvent& e);
    void onLostCapture(wxMouseCaptureLostEvent& e);
    bool held_ = false;
    bool mouse_ = false;    // held by the mouse (rather than the space bar)
    wxString clock_;
};

#endif
