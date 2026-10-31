// The microphone processor deck: see VoiceDeck.h.
#include "VoiceDeck.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>
#include <wx/app.h>

#include "DeckParts.h"
#include "DrawnIcon.h"
#include "Prefs.h"
#include "ragtime/audio/VoiceAudio.h"
#include "ragtime/denoise/RnNoise.h"
#include "ragtime/voice/Voice.h"

using namespace ragtime;

namespace
{

constexpr int TICK_MS = 33;
// The Coding lamp stays lit at least this long, so it can be seen.
constexpr uint64_t CODING_LAMP_MS = 300;
// The Clip lamp stays lit this many ticks after the mic hits full scale.
constexpr int CLIP_HOLD_TICKS = 30;

const wxString DOT = wxString::FromUTF8(" \xc2\xb7 ");
const wxString MINUS = wxString::FromUTF8("\xe2\x88\x92");

wxString utf8(const std::string& s)
{
    return wxString::FromUTF8(s);
}

uint64_t nowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// "−26", "+12", "0" with a true minus.
wxString signedDb(double db, const char* format = "%.0f")
{
    double r = std::fabs(db) < 0.05 ? 0.0 : db;
    wxString n = wxString::Format(format, std::fabs(r));
    if (r < 0) return MINUS + n;
    if (r > 0) return "+" + n;
    return n;
}

deck::KnobSpec spec(double lo, double hi, double step, double def, double plain, double shift, double control)
{
    deck::KnobSpec s;
    s.minimum = lo;
    s.maximum = hi;
    s.step = step;
    s.defaultValue = def;
    s.wheelPlain = plain;
    s.wheelShift = shift;
    s.wheelControl = control;
    return s;
}

DeckLamp::Colour lampColour(voice::Lamp lamp)
{
    switch (lamp)
    {
    case voice::Lamp::Green: return DeckLamp::Colour::Green;
    case voice::Lamp::Amber: return DeckLamp::Colour::Amber;
    case voice::Lamp::Red: break;
    }
    return DeckLamp::Colour::Red;
}

// "sotto v3.6, shape 9-9-8-8" gives "v3.6".
std::string versionOf(const std::string& format)
{
    size_t v = format.find(" v");
    if (v == std::string::npos) return format;
    size_t end = format.find_first_of(", ", v + 1);
    return format.substr(v + 1, end == std::string::npos ? std::string::npos : end - v - 1);
}

wxStaticText* readout(wxWindow* parent, const wxString& text, double points = 8.0)
{
    auto* t = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    t->SetFont(deckart::readoutFont(parent, points));
    t->SetForegroundColour(deckart::colours().dimInk);
    return t;
}

wxStaticText* engraved(wxWindow* parent, const wxString& text, double points = 7.5)
{
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetFont(deckart::captionFont(parent, points));
    t->SetForegroundColour(deckart::colours().ink);
    return t;
}

} // namespace

VoiceDeck::VoiceDeck(wxWindow* parent, VoiceDeckHooks hooks)
    : wxFrame(parent, wxID_ANY, wxString::FromUTF8("Microphone deck \xc2\xb7 Sotto Voce"), wxDefaultPosition,
              wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
    , hooks_(std::move(hooks))
    , sotto_(voice::Sotto::library())
    , timer_(this)
    , alive_(std::make_shared<bool>(true))
{
    loadSettings();
    buildPanel();
    // The knobs as they were left.
    gain_->setValue(settings_.inputGainDb);
    highPass_->setValue(settings_.highPassHz);
    highPass_->setPushed(settings_.highPassIn);
    noise_->setValue(settings_.noiseAmount * 100.0);
    noise_->setPushed(settings_.noiseIn);
    target_->setValue(settings_.targetDb);
    target_->setPushed(!settings_.peakMode);
    review_->SetValue(loadBool(keys::VOICE_DECK_REVIEW, false));
    applySettings();

    // The microphone opens with the deck, so the meters move before you
    // talk.
    std::weak_ptr<bool> alive = alive_;
    hooks_.audio().listen([this, alive](const float* s, size_t n, int rate) {
        if (alive.expired()) return;
        onSamples(s, n, rate);
    });

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_CHAR_HOOK, &VoiceDeck::onCharHook, this);
    bindKeyUp(this);
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
        // Space let go in another window still lets go here.
        if (!e.GetActive() && spaceHeld_)
        {
            spaceHeld_ = false;
            talkUp();
        }
        e.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        timer_.Stop();
        *alive_ = false;
        if (talk_state_.phase() == deck::TalkPhase::Recording) discardClip();
        stopMonitor();
        hooks_.audio().stopListening();
        if (hooks_.holdPlayback) hooks_.holdPlayback(false);
        saveSettings();
        if (hooks_.onClosed) hooks_.onClosed();
        Destroy();
    });
    timer_.Start(TICK_MS);
    tick();
}

VoiceDeck::~VoiceDeck()
{
    *alive_ = false;
}

//-------------------------------------------------------------------------
// The panel
//-------------------------------------------------------------------------

void VoiceDeck::buildPanel()
{
    const deckart::Colours c = deckart::colours();
    auto* plate = new wxPanel(this);
    plate->SetBackgroundColour(c.plate);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // The nameplate.
    auto* head = new wxPanel(plate);
    head->SetBackgroundColour(wxColour(0x1f, 0x1d, 0x1a));
    auto* headSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* mic = new wxStaticBitmap(head, wxID_ANY, microphoneIcon(wxColour(0xe9, 0xdf, 0xc4), 28));
    auto* names = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(head, wxID_ANY, "SOTTO VOCE");
    title->SetFont(deckart::captionFont(head, 13.0).Bold());
    title->SetForegroundColour(wxColour(0xef, 0xe5, 0xca));
    auto* subtitle = new wxStaticText(head, wxID_ANY, wxString::FromUTF8("SPEECH INPUT \xc2\xb7 RAGTIME"));
    subtitle->SetFont(deckart::captionFont(head, 7.0));
    subtitle->SetForegroundColour(wxColour(0xb9, 0xae, 0x92));
    names->Add(title);
    names->Add(subtitle);
    where_ = new wxStaticText(head, wxID_ANY, "");
    where_->SetFont(deckart::readoutFont(head, 9.0));
    where_->SetForegroundColour(wxColour(0xd8, 0xcf, 0xb5));
    gear_ = new wxStaticText(head, wxID_ANY, "");
    gear_->SetFont(deckart::readoutFont(head, 9.0));
    gear_->SetForegroundColour(wxColour(0xd8, 0xcf, 0xb5));
    headSizer->Add(mic, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
    headSizer->Add(names, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    headSizer->AddStretchSpacer();
    headSizer->Add(where_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
    headSizer->Add(gear_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    head->SetSizer(headSizer);
    head->SetMinSize(FromDIP(wxSize(-1, 50)));
    outer->Add(head, 0, wxEXPAND | wxALL, FromDIP(10));

    auto* modules = new wxBoxSizer(wxHORIZONTAL);

    // 1 · Input.
    {
        auto* m = new DeckModule(plate, wxString::FromUTF8("1 \xc2\xb7 INPUT"));
        auto* s = m->content();
        s->Add(engraved(m, "Microphone", 7.5), 0, wxBOTTOM, FromDIP(2));
        microphone_ = new wxChoice(m, wxID_ANY);
        microphone_->SetToolTip("Your computer's microphone. The radio's sound card is never offered.");
        s->Add(microphone_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        auto* meters = new wxBoxSizer(wxHORIZONTAL);
        auto meterColumn = [&](DeckMeter* meter, const wxString& caption, wxStaticText*& value) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            col->Add(meter, 0, wxALIGN_CENTER_HORIZONTAL);
            auto* cap = engraved(m, caption, 7.0);
            col->Add(cap, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));
            value = readout(m, "", 7.5);
            value->SetMinSize(FromDIP(wxSize(108, -1)));
            col->Add(value, 0, wxALIGN_CENTER_HORIZONTAL);
            return col;
        };
        active_ = new DeckMeter(m, DeckMeter::Face::Cream, deck::ACTIVE_FLOOR_DBFS, deck::ACTIVE_TOP_DBFS,
                                {-40, -30, -20, -10, 0},
                                {{deck::ACTIVE_BAND_LOW_DBFS, deck::ACTIVE_BAND_HIGH_DBFS, false}}, FromDIP(wxSize(108, 64)));
        active_->SetToolTip("Active level: your speech level with the pauses left out (ITU-T P.56). "
                            "Aim for the green band around -26 dBFS, where sotto codes best.");
        loudest_ = new DeckMeter(m, DeckMeter::Face::Black, deck::LOUDEST_FLOOR_DBFS, deck::LOUDEST_TOP_DBFS,
                                 {-30, -20, -10, 0},
                                 {{deck::LOUDEST_BAND_LOW_DBFS, deck::LOUDEST_BAND_HIGH_DBFS, false},
                                  {deck::LOUDEST_RED_DBFS, deck::LOUDEST_TOP_DBFS, true}},
                                 FromDIP(wxSize(108, 64)));
        loudest_->SetToolTip("Loudest 10 ms: the RMS of each 10 ms, sotto's own measure. Green is where sotto's "
                             "test voices peak; past -7.3 dBFS (red) sotto's energy field saturates. The orange "
                             "mark holds the loudest for a second.");
        meters->Add(meterColumn(active_, "ACTIVE LEVEL", activeText_), 0, wxRIGHT, FromDIP(6));
        meters->Add(meterColumn(loudest_, "LOUDEST 10 ms", loudestText_), 0);
        s->Add(meters, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(6));
        auto* gainRow = new wxBoxSizer(wxHORIZONTAL);
        gain_ = new DeckKnob(m, wxID_ANY, "GAIN", spec(-20, 40, 0.1, 0, 1, 0.1, 6), FromDIP(wxSize(76, 84)));
        gain_->setFormatter([](double v, bool) { return signedDb(v, std::fabs(v - std::round(v)) > 0.05 ? "%.1f" : "%.0f") + " dB"; });
        clip_ = new DeckLamp(m, "CLIP", DeckLamp::Colour::Red, true);
        clip_->SetToolTip("Lights when the microphone reaches full scale, true clipping. Turn Gain down.");
        gainRow->Add(gain_, 0, wxALIGN_CENTER_VERTICAL);
        gainRow->Add(clip_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        s->Add(gainRow, 0, wxALIGN_CENTER_HORIZONTAL);
        m->SetMinSize(FromDIP(wxSize(240, -1)));
        modules->Add(m, 0, wxEXPAND | wxRIGHT, FromDIP(6));
    }

    // 2 · Clean-up.
    {
        auto* m = new DeckModule(plate, wxString::FromUTF8("2 \xc2\xb7 CLEAN-UP"));
        auto* s = m->content();
        auto stage = [&](const wxString& caption, DeckKnob*& knob, DeckLamp*& lamp, const deck::KnobSpec& ks,
                         const wxString& words, const wxString& tip) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(engraved(m, caption, 7.5), 1, wxALIGN_CENTER_VERTICAL);
            lamp = new DeckLamp(m, "", DeckLamp::Colour::Amber, false);
            row->Add(lamp, 0, wxALIGN_CENTER_VERTICAL);
            s->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
            knob = new DeckKnob(m, wxID_ANY, "", ks, FromDIP(wxSize(120, 82)));
            knob->setNote("push in/out");
            knob->SetToolTip(tip + "\n\n" + knob->GetToolTipText());
            s->Add(knob, 0, wxALIGN_CENTER_HORIZONTAL);
            auto* w = new wxStaticText(m, wxID_ANY, words);
            w->SetFont(w->GetFont().Smaller());
            w->SetForegroundColour(deckart::colours().dimInk);
            s->Add(w, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(4));
        };
        deck::KnobSpec hp;
        hp.positions = {80, 100, 150};
        hp.defaultValue = 100;
        hp.pushable = true;
        hp.defaultPushed = true;
        stage("HIGH-PASS", highPass_, highPassLamp_, hp, "Hum, thumps and pops out",
              "High-pass: 80, 100 or 150 Hz. Hum, desk thumps and breath pops carry energy sotto would spend "
              "bits on. Push it in to switch it in.");
        highPass_->setFormatter([](double v, bool in) { return in ? wxString::Format("%.0f Hz", v) : wxString("Off"); });
        s->Add(new wxStaticLine(m), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
        deck::KnobSpec nz = spec(0, 100, 1, 70, 5, 1, 25);
        nz.pushable = true;
        nz.defaultPushed = true;
        const bool haveNoise = denoise::haveRnNoise();
        stage(haveNoise ? wxString::FromUTF8("NOISE \xc2\xb7 RNNOISE") : wxString("NOISE"), noise_, noiseLamp_, nz,
              haveNoise ? wxString("Fans out, voicing kept") : wxString("Built without RNNoise"),
              "Noise reduction with RNNoise, and how much of it. sotto asks whether a voice's harmonics stand out "
              "from the noise, so a fan behind you turns vowels into hiss: cleaning first protects the voicing. "
              "Push it in to switch it in.");
        noise_->setFormatter([](double v, bool in) { return in ? wxString::Format("%.0f %%", v) : wxString("Off"); });
        if (!haveNoise) noise_->Enable(false);
        m->SetMinSize(FromDIP(wxSize(150, -1)));
        modules->Add(m, 0, wxEXPAND | wxRIGHT, FromDIP(6));
    }

    // 3 · Leveller.
    {
        auto* m = new DeckModule(plate, wxString::FromUTF8("3 \xc2\xb7 LEVELLER"));
        auto* s = m->content();
        deck::KnobSpec ts = spec(-36, -16, 0.5, voice::LEVEL_TARGET_DBFS, 1, 0.5, 3);
        ts.pushable = true;
        ts.defaultPushed = true;
        target_ = new DeckKnob(m, wxID_ANY, "TARGET", ts, FromDIP(wxSize(96, 96)));
        target_->setRingWhenOut(true);
        target_->setFormatter([](double v, bool) {
            return signedDb(v, std::fabs(v - std::round(v)) > 0.05 ? "%.1f" : "%.0f") + " dBFS";
        });
        target_->SetToolTip("Target: the active level the leveller aims for, -26 dBFS by default (sotto's). "
                            "Pushed in, it rides your average level and a limiter catches peaks. Popped out "
                            "(the ring lit), a loud 10 ms burst also turns you down for good, so someone who "
                            "keeps shouting gets levelled rather than limited syllable by syllable.\n\n"
                            + target_->GetToolTipText());
        s->Add(target_, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4));
        s->AddSpacer(FromDIP(8));
        s->Add(engraved(m, "GAIN REDUCTION", 7.0), 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(3));
        reduction_ = new ReductionBar(m);
        s->Add(reduction_, 0, wxEXPAND);
        s->AddStretchSpacer();
        auto* words = new wxStaticText(m, wxID_ANY, "Popped out: loud bursts add reduction. Pushed in: level only",
                                       wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        words->SetFont(words->GetFont().Smaller());
        words->SetForegroundColour(deckart::colours().dimInk);
        words->Wrap(FromDIP(130));
        s->Add(words, 0, wxALIGN_CENTER_HORIZONTAL);
        m->SetMinSize(FromDIP(wxSize(150, -1)));
        modules->Add(m, 0, wxEXPAND | wxRIGHT, FromDIP(6));
    }

    // 4 · sotto.
    {
        auto* m = new DeckModule(plate, wxString::FromUTF8("4 \xc2\xb7 SOTTO"));
        auto* s = m->content();
        scope_ = new PitchScope(m, FromDIP(wxSize(184, 100)));
        s->Add(scope_, 0, wxALIGN_CENTER_HORIZONTAL);
        codec_ = new wxStaticText(m, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        codec_->SetFont(deckart::readoutFont(m, 8.0));
        codec_->SetForegroundColour(deckart::colours().ink);
        s->Add(codec_, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
        s->Add(engraved(m, wxString::FromUTF8("LEVEL CHECK \xc2\xb7 THIS CLIP"), 7.0), 0, wxBOTTOM, FromDIP(3));
        auto* grid = new wxFlexGridSizer(3, FromDIP(2), FromDIP(6));
        grid->AddGrowableCol(2);
        auto row = [&](const wxString& name, DeckLamp*& lamp, wxStaticText*& text, const wxString& tip) {
            lamp = new DeckLamp(m, name, DeckLamp::Colour::Green, false, FromDIP(wxSize(74, 18)));
            lamp->SetToolTip(tip);
            text = new wxStaticText(m, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
            text->SetFont(deckart::readoutFont(m, 7.5));
            text->SetForegroundColour(deckart::colours().dimInk);
            grid->Add(lamp, 0, wxALIGN_CENTER_VERTICAL);
            grid->AddSpacer(0);
            grid->Add(text, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        };
        row("Active", checkActive_, checkActiveText_,
            "The clip's active level: green within 3 dB of -26 dBFS, amber near it, red off it.");
        row("Loudest", checkLoudest_, checkLoudestText_,
            "The clip's loudest 10 ms: green when every 10 ms stayed under -7.3 dBFS and nothing clipped.");
        row("Noise", checkNoise_, checkNoiseText_,
            "The quietest tenth of the clip against the voice: green at least 30 dB below it.");
        s->Add(grid, 0, wxEXPAND);
        m->SetMinSize(FromDIP(wxSize(208, -1)));
        modules->Add(m, 0, wxEXPAND | wxRIGHT, FromDIP(6));
    }

    // 5 · To the air.
    {
        auto* m = new DeckModule(plate, wxString::FromUTF8("5 \xc2\xb7 TO THE AIR"));
        auto* s = m->content();
        s->Add(engraved(m, "Send to", 7.5), 0, wxBOTTOM, FromDIP(2));
        sendTo_ = new wxChoice(m, wxID_ANY);
        sendTo_->SetToolTip("Where the clip goes: the session partner, or the D2G net.");
        s->Add(sendTo_, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        talk_ = new TalkButton(m, FromDIP(wxSize(116, 116)));
        s->Add(talk_, 0, wxALIGN_CENTER_HORIZONTAL);
        monitor_ = new wxButton(m, wxID_ANY, "Monitor", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        monitor_->SetToolTip("Plays your last clip through sotto's encoder and decoder, so you hear what the far "
                             "end will hear. Push to talk stops it.");
        // Monitor, and Send and Discard beside it while a clip is held for
        // review.
        auto* review = new wxBoxSizer(wxHORIZONTAL);
        reviewSend_ = new wxButton(m, wxID_ANY, "Send", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        reviewDiscard_ = new wxButton(m, wxID_ANY, "Discard", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        review->Add(monitor_, 1);
        review->Add(reviewSend_, 1, wxLEFT, FromDIP(4));
        review->Add(reviewDiscard_, 1, wxLEFT, FromDIP(4));
        s->Add(review, 0, wxEXPAND | wxTOP, FromDIP(4));
        s->AddStretchSpacer();
        auto* lamps = new wxGridSizer(4, 0, FromDIP(2));
        recLamp_ = new DeckLamp(m, "REC", DeckLamp::Colour::Red, true, FromDIP(wxSize(52, 32)));
        codingLamp_ = new DeckLamp(m, "CODING", DeckLamp::Colour::Amber, true, FromDIP(wxSize(52, 32)));
        queuedLamp_ = new DeckLamp(m, "QUEUED", DeckLamp::Colour::Amber, true, FromDIP(wxSize(52, 32)));
        sendingLamp_ = new DeckLamp(m, "SENDING", DeckLamp::Colour::Green, true, FromDIP(wxSize(52, 32)));
        for (DeckLamp* l : {recLamp_, codingLamp_, queuedLamp_, sendingLamp_}) lamps->Add(l, 0, wxALIGN_CENTER_HORIZONTAL);
        s->Add(lamps, 0, wxEXPAND);
        modules->Add(m, 1, wxEXPAND);
    }
    outer->Add(modules, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    // On the air: the airtime bar.
    {
        auto* m = new DeckModule(plate, "");
        auto* s = new wxBoxSizer(wxHORIZONTAL);
        auto* left = new wxBoxSizer(wxVERTICAL);
        left->Add(engraved(m, "ON THE AIR", 8.5));
        airGear_ = new wxStaticText(m, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(110, -1)), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        airGear_->SetFont(airGear_->GetFont().Smaller());
        airGear_->SetForegroundColour(deckart::colours().dimInk);
        left->Add(airGear_);
        s->Add(left, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        airtime_ = new AirtimeBar(m);
        s->Add(airtime_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        airWords_ = new wxStaticText(m, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(220, -1)), wxST_NO_AUTORESIZE);
        airWords_->SetFont(deckart::readoutFont(m, 8.5));
        airWords_->SetForegroundColour(deckart::colours().ink);
        s->Add(airWords_, 0, wxALIGN_CENTER_VERTICAL);
        m->content()->Add(s, 1, wxEXPAND);
        outer->Add(m, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    }

    // The strip under the panel: standard widgets.
    {
        auto* strip = new wxBoxSizer(wxHORIZONTAL);
        auto* sp = new wxStaticText(plate, wxID_ANY, "Speaker");
        speaker_ = new wxChoice(plate, wxID_ANY);
        speaker_->SetMaxSize(FromDIP(wxSize(200, -1)));
        speaker_->SetToolTip("Where Monitor and arriving voice play. The radio's sound card is never offered.");
        auto* vol = new wxStaticText(plate, wxID_ANY, "Volume");
        volume_ = new wxSlider(plate, wxID_ANY, 0, -30, 10, wxDefaultPosition, FromDIP(wxSize(110, -1)));
        volume_->SetToolTip("The speaker's volume, in dB");
        arriving_ = new wxCheckBox(plate, wxID_ANY, "Play arriving voice");
        arriving_->SetToolTip("While the deck is open, voice that arrives plays by itself, in the order it came.");
        arriving_->SetValue(loadBool(keys::PLAY_ARRIVING_VOICE, true));
        review_ = new wxCheckBox(plate, wxID_ANY, "Hold my clips for review");
        review_->SetToolTip("Letting go of Push to talk keeps the clip here, with Monitor, Send and Discard, "
                            "instead of sending it at once.");
        status_ = new wxStaticText(plate, wxID_ANY, "Esc while holding throws the clip away", wxDefaultPosition,
                                   wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        status_->SetForegroundColour(deckart::colours().dimInk);
        auto* close = new wxButton(plate, wxID_CLOSE, "Close");
        strip->Add(sp, 0, wxALIGN_CENTER_VERTICAL);
        strip->Add(speaker_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        strip->Add(vol, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        strip->Add(volume_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        strip->Add(arriving_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        strip->Add(review_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        strip->Add(status_, 2, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        strip->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        outer->Add(strip, 0, wxEXPAND | wxALL, FromDIP(10));
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    }
    plate->SetSizer(outer);
    for (wxStaticText* t : {where_, gear_}) t->SetBackgroundColour(wxColour(0x1f, 0x1d, 0x1a));

    // About 1020 x 560: it fits a 1366 x 768 screen.
    const wxSize best = plate->GetBestSize();
    SetMinClientSize(wxSize(std::max(best.x, FromDIP(1000)), best.y));
    SetClientSize(wxSize(std::max(best.x, FromDIP(1020)), best.y));

    // The knobs.
    auto knobChanged = [this](wxCommandEvent&) {
        applySettings();
        saveSettings();
    };
    for (DeckKnob* k : {gain_, highPass_, noise_, target_})
    {
        k->Bind(wxEVT_SLIDER, knobChanged);
        k->Bind(wxEVT_TOGGLEBUTTON, knobChanged);
    }
    talk_->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        if (e.GetInt()) talkDown();
        else talkUp();
    });
    monitor_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggleMonitor(); });
    reviewSend_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (talk_state_.sendReviewed()) sendClip();
    });
    reviewDiscard_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!talk_state_.discardReviewed()) return;
        stopMonitor();
        status_->SetLabel("Clip thrown away");
        lastSot_.clear();
        lastFrames_.clear();
    });
    review_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { saveSettings(); });
    arriving_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { saveBool(keys::PLAY_ARRIVING_VOICE, arriving_->GetValue()); });
    microphone_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        int i = microphone_->GetSelection();
        std::string name = i >= 0 && i < (int)micNames_.size() ? micNames_[(size_t)i] : std::string();
        saveString(keys::VOICE_MICROPHONE, name);
        hooks_.audio().chooseMicrophone(name);
    });
    speaker_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        int i = speaker_->GetSelection();
        std::string name = i >= 0 && i < (int)speakerNames_.size() ? speakerNames_[(size_t)i] : std::string();
        saveString(keys::VOICE_SPEAKER, name);
        hooks_.audio().chooseSpeaker(name);
    });
    volume_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int db = volume_->GetValue();
        saveInt(keys::VOICE_VOLUME, db);
        hooks_.audio().setVolume((float)std::pow(10.0, db / 20.0));
    });
    sendTo_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        showTarget();
        updateAirtime();
    });
}

//-------------------------------------------------------------------------
// Settings
//-------------------------------------------------------------------------

void VoiceDeck::loadSettings()
{
    settings_.inputGainDb = loadInt(keys::VOICE_DECK_GAIN, 0) / 10.0;
    settings_.highPassHz = loadInt(keys::VOICE_DECK_HIGH_PASS, 100);
    settings_.highPassIn = loadBool(keys::VOICE_DECK_HIGH_PASS_IN, true);
    settings_.noiseAmount = loadInt(keys::VOICE_DECK_NOISE, 70) / 100.0;
    settings_.noiseIn = loadBool(keys::VOICE_DECK_NOISE_IN, true);
    settings_.targetDb = loadInt(keys::VOICE_DECK_TARGET, (int)std::lround(voice::LEVEL_TARGET_DBFS * 10)) / 10.0;
    settings_.peakMode = loadBool(keys::VOICE_DECK_PEAKS, false);
    settings_ = settings_.clamped();
}

void VoiceDeck::saveSettings() const
{
    saveInt(keys::VOICE_DECK_GAIN, (int)std::lround(settings_.inputGainDb * 10));
    saveInt(keys::VOICE_DECK_HIGH_PASS, (int)std::lround(settings_.highPassHz));
    saveBool(keys::VOICE_DECK_HIGH_PASS_IN, settings_.highPassIn);
    saveInt(keys::VOICE_DECK_NOISE, (int)std::lround(settings_.noiseAmount * 100));
    saveBool(keys::VOICE_DECK_NOISE_IN, settings_.noiseIn);
    saveInt(keys::VOICE_DECK_TARGET, (int)std::lround(settings_.targetDb * 10));
    saveBool(keys::VOICE_DECK_PEAKS, settings_.peakMode);
    if (review_) saveBool(keys::VOICE_DECK_REVIEW, review_->GetValue());
}

void VoiceDeck::applySettings()
{
    settings_.inputGainDb = gain_->value();
    settings_.highPassHz = highPass_->value();
    settings_.highPassIn = highPass_->pushed();
    settings_.noiseAmount = noise_->value() / 100.0;
    settings_.noiseIn = noise_->pushed() && noise_->IsEnabled();
    settings_.targetDb = target_->value();
    settings_.peakMode = !target_->pushed();
    highPassLamp_->setLit(settings_.highPassIn);
    noiseLamp_->setLit(settings_.noiseIn);
    std::lock_guard<std::mutex> lock(mutex_);
    newSettings_ = true;
}

//-------------------------------------------------------------------------
// The audio thread
//-------------------------------------------------------------------------

void VoiceDeck::onSamples(const float* samples, size_t n, int rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!chain_ || chain_->inputRate() != rate)
    {
        chain_ = std::make_unique<voice::Chain>(rate, denoise::makeRnNoise());
        newSettings_ = true;
    }
    if (newSettings_)
    {
        chain_->setSettings(settings_);
        newSettings_ = false;
    }
    scratch_.clear();
    chain_->process(samples, n, scratch_);
    if (recording_) coded8k_.insert(coded8k_.end(), scratch_.begin(), scratch_.end());
}

//-------------------------------------------------------------------------
// The window's clock
//-------------------------------------------------------------------------

void VoiceDeck::tick()
{
    const double seconds = TICK_MS / 1000.0;
    ticks_++;
    if (ticks_ % 15 == 1)
    {
        showDevices();
        showTarget();
        showCodec();
    }
    showMeters(seconds);
    drainCoded();
    if (talk_state_.phase() == deck::TalkPhase::Recording)
        talk_->setClock(utf8(deck::clockText((nowMs() - recordStartMs_) / 1000.0)));
    showClipLamps();
}

void VoiceDeck::showDevices()
{
    audio::VoiceStatus st = hooks_.audio().status();
    auto fill = [](wxChoice* choice, std::vector<std::string>& names, const std::vector<ear::GraphNode>& nodes,
                   const std::string& chosen) {
        std::vector<std::string> now{std::string()};
        wxArrayString labels;
        labels.Add("System default");
        for (const ear::GraphNode& n : nodes)
        {
            now.push_back(n.name);
            labels.Add(utf8(voice::label(n)));
        }
        if (!chosen.empty() && std::find(now.begin(), now.end(), chosen) == now.end())
        {
            now.push_back(chosen);
            labels.Add(utf8(chosen) + " (not here)");
        }
        if (now != names)
        {
            names = now;
            choice->Set(labels);
        }
        int at = (int)(std::find(names.begin(), names.end(), chosen) - names.begin());
        if (at < (int)names.size() && choice->GetSelection() != at) choice->SetSelection(at);
    };
    fill(microphone_, micNames_, st.devices.microphones, st.chosenMicrophone);
    fill(speaker_, speakerNames_, st.devices.speakers, st.chosenSpeaker);
    if (volume_->GetValue() != loadInt(keys::VOICE_VOLUME, 0)) volume_->SetValue(loadInt(keys::VOICE_VOLUME, 0));

    // Why the deck can't record, if it can't.
    wxString why;
    if (!st.available) why = utf8(st.problem.empty() ? "No sound system to record from" : st.problem);
    else if (!st.microphone.usable()) why = utf8(voice::describe(st.microphone, true));
    else if (!st.microphoneProblem.empty()) why = utf8(st.microphoneProblem);
    else if (!sotto_.available()) why = "Voice can't be coded: " + utf8(sotto_.info().why);
    bool canTalk = why.empty();
    if (talk_->IsEnabled() != canTalk && talk_state_.phase() != deck::TalkPhase::Recording) talk_->Enable(canTalk);
    // The line says why it can't, or which microphone it uses until a
    // clip has something to say.
    const wxString line = canTalk ? utf8(voice::describe(st.microphone, true)) : why;
    if (line != deviceLine_ && (status_->GetLabel().empty() || status_->GetLabel() == deviceLine_ || !canTalk))
        status_->SetLabel(line);
    deviceLine_ = line;
}

bool VoiceDeck::sendToBroadcast() const
{
    // The last entry is always the net.
    return targetNow_.peer.empty() || sendTo_->GetSelection() == (int)sendTo_->GetCount() - 1;
}

void VoiceDeck::showTarget()
{
    if (hooks_.target) targetNow_ = hooks_.target();
    wxArrayString items;
    if (!targetNow_.peer.empty()) items.Add(utf8(targetNow_.peer) + " (session)");
    items.Add("D2G net");
    bool same = items.size() == sendTo_->GetCount();
    for (size_t i = 0; same && i < items.size(); i++) same = items[i] == sendTo_->GetString((unsigned)i);
    if (!same)
    {
        sendTo_->Set(items);
        // A session, when one starts, is where clips go.
        sendTo_->SetSelection(0);
    }
    if (sendTo_->GetSelection() < 0) sendTo_->SetSelection(0);
    const bool broadcast = sendToBroadcast();
    where_->SetLabel(broadcast ? wxString("To the D2G net") : "Session with " + utf8(targetNow_.peer));
    const link::ModeInfo& mode = broadcast ? targetNow_.broadcastMode : targetNow_.sessionMode;
    const std::string& gearWords = broadcast ? targetNow_.broadcastGear : targetNow_.sessionGear;
    wxString gear = utf8(gearWords);
    if (mode.valid())
    {
        // The gear's words often name the mode already.
        if (!gear.Lower().Contains(utf8(mode.name).Lower())) gear += (gear.empty() ? wxString() : DOT) + utf8(mode.name);
        if (mode.secondsAtMax > 0)
        {
            double bps = 8.0 * mode.bytesPerCodeword * mode.maxCodewords / mode.secondsAtMax;
            gear += (gear.empty() ? wxString() : DOT) + wxString::Format("%.0f bit/s", bps);
        }
    }
    gear_->SetLabel(gear.empty() ? wxString("the host picks the gear") : gear);
    airGear_->SetLabel(gearWords.empty() ? wxString("") : "at " + utf8(gearWords));
    airGear_->SetToolTip(airGear_->GetLabel());
    gear_->GetParent()->Layout();
}

void VoiceDeck::showMeters(double seconds)
{
    voice::Readings r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chain_)
        {
            r = chain_->takeReadings();
            have = true;
        }
    }
    activeNeedle_.step(have ? r.inputActiveDb : voice::LEVEL_FLOOR_DBFS, seconds);
    loudestNeedle_.step(have ? r.inputLoudestDb : voice::LEVEL_FLOOR_DBFS, seconds);
    active_->setNeedle(activeNeedle_.position());
    loudest_->setNeedle(loudestNeedle_.position());
    loudest_->setHold(loudestNeedle_.hold());
    wxString a = have && r.inputActiveDb > deck::ACTIVE_FLOOR_DBFS
                     ? utf8(deck::dbText(r.inputActiveDb, deck::ACTIVE_FLOOR_DBFS)) + " dBFS"
                     : wxString::FromUTF8("\xe2\x80\x94");
    if (activeText_->GetLabel() != a) activeText_->SetLabel(a);
    wxString l = utf8(deck::dbText(loudestNeedle_.position(), deck::LOUDEST_FLOOR_DBFS)) + DOT + "hold "
                 + utf8(deck::dbText(loudestNeedle_.hold(), deck::LOUDEST_FLOOR_DBFS));
    if (loudestText_->GetLabel() != l) loudestText_->SetLabel(l);
    if (have && r.clipped) clipTicks_ = CLIP_HOLD_TICKS;
    clip_->setLit(clipTicks_ > 0);
    if (clipTicks_ > 0) clipTicks_--;
    reduction_->setReduction(have ? std::max(0.0, -r.levellerGainDb) + r.limiterReductionDb : 0.0);
}

void VoiceDeck::showCodec()
{
    wxString text;
    if (!sotto_.available())
        text = "sotto not loaded";
    else
    {
        const std::vector<voice::SottoFrame>& f =
            talk_state_.phase() == deck::TalkPhase::Recording ? frames_ : lastFrames_;
        text = wxString::Format("%d", (int)sotto_.info().codec) + DOT + utf8(versionOf(sotto_.info().format)) + DOT
               + wxString::Format("%zu frames", f.size());
        size_t bytes = talk_state_.phase() == deck::TalkPhase::Recording ? f.size() * voice::FRAME_BYTES : lastSot_.size();
        if (bytes) text += DOT + wxString::Format("%.1f kB", bytes / 1000.0);
    }
    if (codec_->GetLabel() != text) codec_->SetLabel(text);
    codec_->SetToolTip(sotto_.available() ? utf8(sotto_.info().format + " (" + sotto_.info().path + "), 2,400 bit/s")
                                          : utf8(sotto_.info().why));
}

void VoiceDeck::showLevelCheck(const voice::LevelReport& report, bool have)
{
    auto set = [&](DeckLamp* lamp, wxStaticText* text, voice::Lamp colour, const wxString& words) {
        lamp->setColour(lampColour(colour));
        lamp->setLit(have);
        text->SetLabel(have ? words : wxString());
    };
    set(checkActive_, checkActiveText_, report.active,
        signedDb(report.activeDb) + "  target " + signedDb(settings_.targetDb));
    set(checkLoudest_, checkLoudestText_, report.loudest,
        signedDb(report.loudestDb) + "  " + (report.clippedSamples ? wxString("clipped!") : "under " + signedDb(-7.0)));
    set(checkNoise_, checkNoiseText_, report.noise, wxString::Format("%.0f dB below voice", report.marginDb));
}

void VoiceDeck::showClipLamps()
{
    const uint64_t now = nowMs();
    const deck::TalkPhase phase = talk_state_.phase();
    recLamp_->setLit(phase == deck::TalkPhase::Recording);
    codingLamp_->setLit(phase == deck::TalkPhase::Coding || (codingSinceMs_ && now - codingSinceMs_ < CODING_LAMP_MS));
    if (!lastPath_.empty() && hooks_.clipAir && ticks_ % 5 == 0) lastAir_ = hooks_.clipAir(lastPath_);
    queuedLamp_->setLit(lastAir_ == deck::ClipAir::Queued);
    sendingLamp_->setLit(lastAir_ == deck::ClipAir::Sending);
    talk_->setHeld(phase == deck::TalkPhase::Recording);
    const bool review = phase == deck::TalkPhase::Review;
    if (reviewSend_->IsShown() != review)
    {
        reviewSend_->Show(review);
        reviewDiscard_->Show(review);
        reviewSend_->GetParent()->Layout();
    }
    const bool canMonitor = talk_state_.canMonitor(!lastFrames_.empty()) && sotto_.available();
    if (monitor_->IsEnabled() != (canMonitor || talk_state_.monitoring())) monitor_->Enable(canMonitor || talk_state_.monitoring());
    wxString label = talk_state_.monitoring() ? "Stop" : "Monitor";
    if (monitor_->GetLabel() != label) monitor_->SetLabel(label);
}

//-------------------------------------------------------------------------
// Push to talk
//-------------------------------------------------------------------------

void VoiceDeck::talkDown()
{
    if (!talk_->IsEnabled()) return;
    const bool wasReview = talk_state_.phase() == deck::TalkPhase::Review;
    if (talk_state_.phase() == deck::TalkPhase::Recording || talk_state_.phase() == deck::TalkPhase::Coding) return;
    encoder_ = sotto_.encoder();
    if (!encoder_)
    {
        status_->SetLabel("Voice can't be coded: " + utf8(sotto_.info().why));
        return;
    }
    // Monitor stops at once, and others' voice waits.
    if (talk_state_.press()) hooks_.audio().stopPlaying();
    if (hooks_.holdPlayback) hooks_.holdPlayback(true);
    if (wasReview) status_->SetLabel("The clip held for review was thrown away");
    else status_->SetLabel("Recording");
    frames_.clear();
    scope_->clear();
    showLevelCheck(voice::LevelReport(), false);
    recordStartMs_ = nowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coded8k_.clear();
        if (chain_) chain_->startClip();
        recording_ = true;
    }
    showClipLamps();
}

void VoiceDeck::talkUp()
{
    if (!talk_state_.release()) return;
    codingSinceMs_ = nowMs();
    showClipLamps();
    finishClip();
}

void VoiceDeck::discardClip()
{
    if (!talk_state_.discard()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        coded8k_.clear();
        if (chain_)
        {
            std::vector<float> drop;
            chain_->finish(drop);
        }
    }
    encoder_.reset();
    frames_.clear();
    scope_->clear();
    talk_->setClock("");
    if (hooks_.holdPlayback) hooks_.holdPlayback(false);
    status_->SetLabel("Clip thrown away");
    updateAirtime();
    showClipLamps();
}

void VoiceDeck::drainCoded()
{
    if (talk_state_.phase() != deck::TalkPhase::Recording || !encoder_) return;
    std::vector<float> pcm;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm.swap(coded8k_);
    }
    if (pcm.empty()) return;
    std::vector<int16_t> samples(pcm.size());
    voice::toPcm16(pcm.data(), pcm.size(), samples.data());
    size_t before = frames_.size();
    encoder_->push(samples.data(), samples.size(), frames_);
    for (size_t i = before; i < frames_.size(); i++)
    {
        const voice::Frame& f = frames_[i];
        bool voiced = !voice::isSilent(f) && voice::voicingOf(f) != voice::Voicing::Unvoiced;
        scope_->add(voiced ? voice::pitchHzOf(f) : 0.0, voice::energyDbfsOf(f));
    }
    if (frames_.size() != before) updateAirtime();
}

void VoiceDeck::finishClip()
{
    voice::LevelReport report;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        if (chain_)
        {
            // The chain's last 30 ms or so, then the clip's Level check.
            chain_->finish(coded8k_);
            report = chain_->levelReport();
        }
    }
    // The encoder's look-ahead, then the clip is whole.
    {
        std::vector<float> pcm;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pcm.swap(coded8k_);
        }
        if (encoder_)
        {
            std::vector<int16_t> samples(pcm.size());
            voice::toPcm16(pcm.data(), pcm.size(), samples.data());
            encoder_->push(samples.data(), samples.size(), frames_);
            encoder_->flush(frames_);
        }
    }
    encoder_.reset();
    talk_->setClock("");
    if (hooks_.holdPlayback) hooks_.holdPlayback(false);

    std::vector<uint8_t> sot = voice::makeSot(frames_);
    const bool empty = sot.empty();
    showLevelCheck(report, report.speech);
    talk_state_.coded(review_->GetValue(), empty);
    if (empty)
    {
        status_->SetLabel(frames_.size() * voice::FRAME_MS > voice::MAX_CLIP_SECONDS * 1000ull
                              ? "Too long to send"
                              : "Nothing but silence: nothing sent");
        frames_.clear();
        updateAirtime();
        showClipLamps();
        return;
    }
    lastFrames_ = frames_;
    lastSot_ = std::move(sot);
    updateAirtime();
    if (talk_state_.phase() == deck::TalkPhase::Review)
    {
        // Held: Monitor, Send or Discard.
        status_->SetLabel("Held for review: Monitor to hear it, then Send or Discard");
        showClipLamps();
        return;
    }
    sendClip();
}

void VoiceDeck::sendClip()
{
    if (lastSot_.empty() || !hooks_.send) return;
    const bool broadcast = sendToBroadcast();
    std::string error;
    std::string path = hooks_.send(lastSot_, broadcast, error);
    if (path.empty())
    {
        status_->SetLabel(utf8(error.empty() ? "The clip couldn't be sent" : error));
        return;
    }
    lastPath_ = path;
    lastAir_ = deck::ClipAir::Queued;
    double speech = lastFrames_.size() * voice::FRAME_MS / 1000.0;
    status_->SetLabel(wxString::Format("Sent %s of speech, %.1f kB, ", utf8(deck::clockText(speech)), lastSot_.size() / 1000.0)
                      + (broadcast ? wxString("to the net") : "to " + utf8(targetNow_.peer)));
    showClipLamps();
}

void VoiceDeck::updateAirtime()
{
    deck::AirBar bar;
    bar.maxSeconds = std::max(voice::MIN_MAX_AIR_SECONDS, loadInt(keys::VOICE_MAX_AIR, voice::DEFAULT_MAX_AIR_SECONDS));
    const bool recording = talk_state_.phase() == deck::TalkPhase::Recording;
    const std::vector<voice::SottoFrame>& frames = recording ? frames_ : lastFrames_;
    bar.speechSeconds = frames.size() * voice::FRAME_MS / 1000.0;
    uint64_t bytes = recording ? voice::makeSot(frames).size() : lastSot_.size();
    if (bytes)
    {
        if (sendToBroadcast())
        {
            if (targetNow_.broadcastMode.valid())
                bar.airSeconds = voice::broadcastAirSeconds(bytes, "voice.sot", targetNow_.callsign, targetNow_.broadcastMode);
        }
        else
            bar.airSeconds = voice::sessionAirSeconds(bytes, targetNow_.sessionBytesPerSecond);
    }
    airtime_->set(bar);
    wxString words = bytes ? utf8(bar.words()) : wxString();
    if (bytes && bar.airSeconds <= 0) words = utf8(deck::clockText(bar.speechSeconds)) + " of speech; airtime unknown";
    if (airWords_->GetLabel() != words) airWords_->SetLabel(words);
}

//-------------------------------------------------------------------------
// Monitor
//-------------------------------------------------------------------------

void VoiceDeck::toggleMonitor()
{
    if (talk_state_.monitoring())
    {
        stopMonitor();
        return;
    }
    if (!talk_state_.startMonitor(!lastFrames_.empty())) return;
    auto dec = sotto_.decoder();
    if (!dec)
    {
        talk_state_.stopMonitor();
        return;
    }
    // The clip as it goes: read back from its file, silences and all.
    voice::Clip clip;
    std::vector<voice::SottoFrame> frames = lastFrames_;
    std::vector<uint8_t> good;
    if (voice::readSot(lastSot_, clip) == voice::ReadResult::Ok)
    {
        frames.assign(clip.frames.begin(), clip.frames.end());
        good = clip.good;
    }
    std::vector<float> sound = voice::toFloat(dec->decodeClip(frames, good));
    std::weak_ptr<bool> alive = alive_;
    hooks_.audio().play(std::move(sound), voice::SOTTO_RATE, [this, alive](bool) {
        wxTheApp->CallAfter([this, alive]() {
            if (alive.expired() || !*alive.lock()) return;
            talk_state_.stopMonitor();
            showClipLamps();
        });
    });
    showClipLamps();
}

void VoiceDeck::stopMonitor()
{
    if (!talk_state_.monitoring()) return;
    talk_state_.stopMonitor();
    hooks_.audio().stopPlaying();
    showClipLamps();
}

//-------------------------------------------------------------------------
// Keys
//-------------------------------------------------------------------------

bool VoiceDeck::spaceIsTalk() const
{
    // A dropdown, box, button or slider with focus keeps its own Space.
    wxWindow* focus = wxWindow::FindFocus();
    if (!focus) return true;
    return !(wxDynamicCast(focus, wxChoice) || wxDynamicCast(focus, wxCheckBox) || wxDynamicCast(focus, wxButton)
             || wxDynamicCast(focus, wxSlider));
}

void VoiceDeck::onCharHook(wxKeyEvent& e)
{
    const int key = e.GetKeyCode();
    if (key == WXK_ESCAPE && talk_state_.phase() == deck::TalkPhase::Recording)
    {
        spaceHeld_ = false;
        discardClip();
        return;
    }
    if (key == WXK_SPACE && spaceIsTalk())
    {
        // Held down, the key repeats: only the first press counts.
        if (!spaceHeld_ && talk_state_.phase() != deck::TalkPhase::Recording)
        {
            spaceHeld_ = true;
            talkDown();
        }
        return;
    }
    e.Skip();
}

void VoiceDeck::onKeyUp(wxKeyEvent& e)
{
    if (e.GetKeyCode() == WXK_SPACE && spaceHeld_)
    {
        spaceHeld_ = false;
        talkUp();
        return;
    }
    e.Skip();
}

void VoiceDeck::bindKeyUp(wxWindow* w)
{
    w->Bind(wxEVT_KEY_UP, &VoiceDeck::onKeyUp, this);
    for (wxWindow* child : w->GetChildren()) bindKeyUp(child);
}
