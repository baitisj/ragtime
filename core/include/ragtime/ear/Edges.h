// Edges: where the rig's filter really cuts, measured from the band's noise
// in the Ear's rows (docs/STAGE1.md, "The Ear", step 2).
//
// Between signals, the receiver's audio is the band's noise through the
// rig's filter: flat across the passband and falling away at its edges.
// So the Ear averages the quiet rows and finds where that shape drops
// DROP_DB below the passband, which is how a filter's width is quoted.
//
// Quiet rows: not heard while we transmit, and not lifted by a signal. A
// row's passband level is its 90th percentile (a carrier or two fill too
// few bins to move it); a row more than BURST_DB over the quieter rows of
// the last half minute has a burst or a strong station in it, and is left
// out. The quiet rows are averaged as power over about AVERAGE_S seconds.
//
// The passband is the run of bins within DROP_DB of its level, the level
// being the median of the bins within 10 dB of the average's 90th
// percentile, so a 500 Hz filter and a 3 kHz one are found alike. Short
// dips (a gap between carriers) up to GAP_HZ don't end it, nor a dip of
// any width inside the span setBridge names: where the rig's notch is
// (a 340 Hz hole on Jeff's IC-7100, docs/STAGE1.md, "The notch"). Its
// ends are interpolated between bins. An end within END_HZ of 0 Hz or of
// the top of the picture is past what the Ear shows, and given as none.
// When the stopband isn't at least STOP_DB under the passband there is no
// clear filter shape (the filter is wider than the picture, or the noise
// is the sound card's own), and there are no edges.
//
// A notch shows as a dip in the average: dip() finds the deepest one in a
// span, DIP_DB or more under the passband, which is where the notch
// really sits.
//
// After the Ear writes the filter, restart() can hold the average off
// for a while: Jeff's IC-7100 swells about 10 dB across the band for 5 to
// 7 s after each knob turn (its AGC, measured 2026-10-26), which would
// otherwise read as the passband.
//
// Plain arithmetic on rows from ear::Spectrum; the window feeds it.
#ifndef RAGTIME_EAR_EDGES_H
#define RAGTIME_EAR_EDGES_H

#include <deque>
#include <optional>
#include <vector>

namespace ragtime
{
namespace ear
{

struct Edges
{
    // The -6 dB points, in Hz; none past what the Ear shows.
    std::optional<double> lowHz;
    std::optional<double> highHz;
    // The passband's level and the stopband's, in dB.
    double passbandDb = 0;
    double stopbandDb = 0;
    // Seconds of quiet the average holds.
    double quietSeconds = 0;
};

// A dip in the passband: a notch.
struct Dip
{
    double hz = 0;
    double depthDb = 0;     // under the passband's level
};

class EdgeFinder
{
public:
    static constexpr double DROP_DB = 6.0;
    static constexpr double BURST_DB = 3.0;
    static constexpr double AVERAGE_S = 10.0;
    static constexpr double HISTORY_S = 30.0;
    // Seconds of quiet before any edges are given.
    static constexpr double NEEDED_S = 3.0;
    static constexpr double GAP_HZ = 60.0;
    static constexpr double END_HZ = 40.0;
    static constexpr double STOP_DB = 10.0;
    static constexpr double DIP_DB = 15.0;

    explicit EdgeFinder(double rowsPerSecond = 10.0);

    // A new speed or span (Spectrum's topHz) starts afresh.
    void setRowsPerSecond(double rowsPerSecond);
    void setTopHz(double topHz);
    double topHz() const { return topHz_; }
    void clear();
    // The filter changed: the average starts afresh, but what it knows of
    // how loud the quiet is stays. Rows in the next holdSeconds go into
    // nothing (the rig's AGC settling after a write).
    void restart(double holdSeconds = 0);
    // Where the notch is, roughly: a dip of any width between these
    // doesn't end the passband. Two equal values (or none) clear it.
    void setBridge(double lowHz, double highHz);

    // One of Spectrum's rows; rows heard while transmitting only count
    // toward nothing.
    void addRow(const std::vector<float>& row, bool transmitting);

    // The edges, once NEEDED_S of quiet rows are in; none without a clear
    // filter shape.
    std::optional<Edges> edges() const;
    // The deepest dip between lowHz and highHz, at least DIP_DB under the
    // passband, once NEEDED_S of quiet rows are in.
    std::optional<Dip> dip(double lowHz, double highHz) const;
    // The quiet rows' average, in dB, or empty.
    std::vector<float> average() const;
    // The passband runs on past the top of the picture: the top bins are
    // within DROP_DB of its level (FM's audio, wider than a 4 kHz span).
    // Nothing until NEEDED_S of quiet.
    bool reachesTop() const;
    // Seconds of quiet rows in the average (capped at what it remembers).
    double quietSeconds() const;
    // Seconds since the last row with a signal in it, or heard while
    // transmitting: how long the band has been quiet.
    double quietRunSeconds() const { return quietRun_ / rowsPerSecond_; }
    // Holding off after restart(): the rig's AGC is settling, so whether
    // the band is quiet can't be told.
    bool settling() const { return holdRows_ > 0; }

private:
    double binHz(int i) const;
    // The average smoothed a little, in dB, which bins have audio, and the
    // passband's level; false with too little to go on.
    bool shape(std::vector<float>& db, std::vector<bool>& heard, double& passband) const;

    double rowsPerSecond_;
    double topHz_;
    std::vector<double> power_;     // the average, as power
    double weight_ = 0;             // quiet rows in it, capped
    std::deque<float> levels_;      // recent rows' passband levels
    double quietRun_ = 0;           // quiet rows in a row
    double holdRows_ = 0;           // rows still to leave out after a write
    double bridgeLowHz_ = 0;
    double bridgeHighHz_ = 0;
};

// The pth percentile (0..1) of some values.
float percentile(std::vector<float> values, double p);

} // namespace ear
} // namespace ragtime

#endif
