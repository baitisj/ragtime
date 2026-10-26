// AutoSpan: how high the Ear's picture goes (docs/STAGE1.md, "The Ear",
// "Span"). Jeff, 2026-10-26: no black space on the right when the filter
// stops short of 4 kHz, and room for FM's wider audio on 2 m and 70 cm.
//
// The operator picks 4, 8 or 12 kHz, or Auto. Auto fits the picture to
// what the radio passes: the high edge the noise shows plus MARGIN_HZ,
// rounded up to STEP_HZ (a 3000 Hz filter whose edge is at 3050 Hz gets
// 3.5 kHz). Until the edges are measured it goes by the rig's mode: 8 kHz
// for FM and AM, 4 kHz for the rest. A high edge past the picture (the
// passband runs off the right) widens it to the next of 4, 8 and 12 kHz;
// on FM and AM so does a passband that reaches the top with no edges at
// all (EdgeFinder::reachesTop), as FM's audio does. Not on SSB, where
// that is more likely the sound card's own flat noise.
// It widens at once but narrows only once the narrower span has held for
// NARROW_MS, so dragging an edge about doesn't make the axis jump.
//
// The caller starts the edges afresh whenever the span changes, since the
// rows' bins change with it. The time is passed in, for tests.
#ifndef RAGTIME_EAR_AUTO_SPAN_H
#define RAGTIME_EAR_AUTO_SPAN_H

#include <cstdint>
#include <optional>
#include <string>

#include "ragtime/ear/Edges.h"

namespace ragtime
{
namespace ear
{

// FM's receive filter is the IF's, about 15 kHz, not the audio's, and its
// PBT does nothing: the Ear neither draws its width nor sets it.
bool fmMode(const std::string& mode);
// FM and AM: audio wider than SSB's.
bool wideAudioMode(const std::string& mode);

class AutoSpan
{
public:
    static constexpr double MARGIN_HZ = 400.0;
    static constexpr double STEP_HZ = 500.0;
    static constexpr uint64_t NARROW_MS = 10000;
    // The fixed choices, and Auto's steps when the passband runs off.
    static constexpr double CHOICES_HZ[] = {4000.0, 8000.0, 12000.0};

    // The span Auto wants for a high edge.
    static double fit(double highHz);
    // The span Auto starts from for a mode (empty: not known).
    static double forMode(const std::string& mode);

    // The span to show now. fixedHz: the operator's choice, 0 for Auto.
    // mode: the rig's, if known. edges, reachesTop: as measured at the
    // span shown now. maxHz: half the sound card's rate.
    double update(uint64_t nowMs, double fixedHz, const std::optional<std::string>& mode,
                  const std::optional<Edges>& edges, bool reachesTop, double maxHz);
    double topHz() const { return top_; }

private:
    double top_ = 0;
    std::optional<std::string> mode_;
    std::optional<double> narrower_;    // waiting to narrow to this
    uint64_t narrowerSinceMs_ = 0;
};

} // namespace ear
} // namespace ragtime

#endif
