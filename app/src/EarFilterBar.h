#ifndef RAGTIME_EAR_FILTER_BAR_H
#define RAGTIME_EAR_FILTER_BAR_H

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <wx/panel.h>

#include "ragtime/ear/Edges.h"
#include "ragtime/ear/FilterControl.h"
#include "ragtime/radio/RigFilter.h"

class ClockDial;
class EarScope;
class PresetButton;
class wxButton;
class wxStaticText;

// What the Ear knows of the rig, asked twice a second (MainFrame fills it
// from RigControl).
struct EarRig
{
    std::string via;    // "rigctld", "Hamlib"; empty with no rig chosen
    std::optional<ragtime::radio::RigFilter> filter;
    // The band's noise from the S-meter, in dB over S9 (the gearshift's QRN).
    std::optional<double> noiseDb;
    // "Let the Ear change the rig's filters" is ticked, with a rig chosen.
    bool allowed = false;
    // The Ear may set the filter (the box ticked, the rig open or rigctld
    // answering).
    bool canSet = false;
    // Why the rig refused the last write, if it did.
    std::string problem;
    // The rig, for presets and calibrations (RigControl::rigKey).
    std::string key;
};

// The Ear's filter controls (docs/STAGE1.md, "The Ear", step 3): the
// presets along the top (a tap recalls, a hold stores), Calibrate and
// Revert, and under them a line saying what is changing,
// with the clock beside Undo and Keep.
//
// It drives ear::FilterEdit (MainFrame's, so that quitting can put the
// rig back after the Ear has closed): a drag on the scope or a preset is a
// change, written once the band is quiet, then on trial for 15 s. A change
// that cuts into Data2G's passband needs Keep pressed twice. Calibrate
// (ear::Calibration) turns the knobs, measures, and keeps what it learned
// (ear::KnobModel) for the rig, mode and width. The write hook goes to
// RigControl, which writes through Hamlib or rigctld, only with the box
// ticked.
//
// The notch's bar on the scope is a change like any other: dragged into
// the spectrum it turns the notch on there, dragged to its bay off. Where
// the noise shows the notch's dip, ear::NotchModel learns from it, kept
// for the rig and mode.
class EarFilterBar : public wxPanel
{
public:
    using Write = std::function<void(const ragtime::radio::RigFilter& filter, const std::string& why)>;

    EarFilterBar(wxWindow* parent, EarScope* scope, ragtime::ear::FilterEdit& edit, Write write);

    // Every tick of the Ear's timer. quiet: not keyed and no burst heard
    // for a moment.
    void tick(uint64_t nowMs, bool quiet, ragtime::ear::EdgeFinder& edges, const EarRig& rig, bool narrow);
    // The Ear is closing: an unkept change goes back, and Calibrate stops.
    void closing();

private:
    void dragged(const ragtime::ear::Span& from, const ragtime::ear::Span& wanted);
    void notchMoved(std::optional<double> hz);
    // The notch as the scope draws it, the dip the noise shows, and what
    // the model learns from it.
    void showNotch(ragtime::ear::EdgeFinder& edges);
    // The notch model for the rig and mode now.
    const ragtime::ear::NotchModel& notchModel();
    // Whether the setting on trial or waiting cuts into Data2G's passband.
    bool cutsData2G() const;
    void recall(int n);
    void store(int n);
    void calibrate();
    void runCalibration(uint64_t nowMs, bool quiet, ragtime::ear::EdgeFinder& edges);
    void write(const ragtime::radio::RigFilter& f, const std::string& why, ragtime::ear::EdgeFinder& edges);
    void show(uint64_t nowMs, const ragtime::ear::EdgeFinder& edges);
    // Calibrate's model for this setting, or the centre and width the Ear
    // goes by until it has run.
    ragtime::ear::KnobModel model(const ragtime::radio::RigFilter& f) const;
    bool calibrated() const;
    // The rig is on FM: its width is the IF filter's and PBT does nothing,
    // so the Ear sets nothing.
    bool onFm() const;
    // The Ear may set the filter now.
    bool settable() const { return rig_.canSet && !onFm(); }
    std::optional<ragtime::ear::Span> spanOfFilter(const ragtime::radio::RigFilter& f) const;
    std::optional<ragtime::ear::Span> measured(const ragtime::ear::EdgeFinder& edges) const;
    std::optional<ragtime::radio::RigFilter> setting() const;
    std::optional<ragtime::ear::Span> spanNow() const;
    void finishCalibration();
    void showPresets();
    void disableFilters();
    void change(const ragtime::radio::RigFilter& to, const std::optional<ragtime::ear::Span>& wanted);

    EarScope* scope_;
    ragtime::ear::FilterEdit& edit_;
    Write write_;
    PresetButton* disable_ = nullptr;
    std::array<PresetButton*, 4> presets_{};
    std::array<std::optional<ragtime::radio::RigFilter>, 4> presetFilters_{};   // what each holds
    wxButton* calibrate_ = nullptr;
    wxButton* putBack_ = nullptr;
    wxStaticText* line_ = nullptr;
    ClockDial* clock_ = nullptr;
    wxButton* undo_ = nullptr;
    wxButton* keep_ = nullptr;

    EarRig rig_;
    bool narrow_ = false;
    uint64_t nowMs_ = 0;
    ragtime::ear::EdgeFinder* edges_ = nullptr;     // this tick's
    std::optional<ragtime::radio::RigFilter> lastFilter_;
    std::optional<double> centre_;          // the passband's, knobs centred
    std::optional<ragtime::ear::Span> from_;    // before the change
    std::optional<ragtime::ear::Span> wanted_;  // what the change was for
    bool keepArmed_ = false;                // a cut into Data2G: Keep again
    std::string miss_;                      // where the edges really went
    uint64_t writtenMs_ = 0;
    std::optional<ragtime::radio::RigFilter> restore_;  // to write once quiet
    mutable std::string calibrationKey_;    // model()'s cache: rig, mode, width
    mutable std::optional<ragtime::ear::KnobModel> calibrated_;
    std::string presetsKey_;                // the presets shown: rig and mode
    bool trialShown_ = false;               // the clock, Undo and Keep are showing

    ragtime::ear::Calibration calibration_;
    std::optional<ragtime::radio::RigFilter> calibrationFrom_;
    bool calibrationWritten_ = false;
    uint64_t calibrationStepMs_ = 0;
    std::string calibrationNews_;           // its outcome, until the next change
    int ticks_ = 0;

    std::string notchKey_;                  // notchModel_'s rig and mode
    ragtime::ear::NotchModel notchModel_;
    std::optional<double> dipHz_;           // where the noise shows the notch
    std::optional<double> learnedRaw_;      // the position the model last learned at
};

#endif
