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
// dips (a notch, a gap between carriers) up to GAP_HZ don't end it. Its
// ends are interpolated between bins. An end within END_HZ of 0 Hz or of
// the top of the picture is past what the Ear shows, and given as none.
// When the stopband isn't at least STOP_DB under the passband there is no
// clear filter shape (the filter is wider than the picture, or the noise
// is the sound card's own), and there are no edges.
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

    explicit EdgeFinder(double rowsPerSecond = 10.0);

    // A new speed starts afresh.
    void setRowsPerSecond(double rowsPerSecond);
    void clear();
    // The filter changed: the average starts afresh, but what it knows of
    // how loud the quiet is stays.
    void restart();

    // One of Spectrum's rows; rows heard while transmitting only count
    // toward nothing.
    void addRow(const std::vector<float>& row, bool transmitting);

    // The edges, once NEEDED_S of quiet rows are in; none without a clear
    // filter shape.
    std::optional<Edges> edges() const;
    // The quiet rows' average, in dB, or empty.
    std::vector<float> average() const;
    // Seconds of quiet rows in the average (capped at what it remembers).
    double quietSeconds() const;
    // Seconds since the last row with a signal in it, or heard while
    // transmitting: how long the band has been quiet.
    double quietRunSeconds() const { return quietRun_ / rowsPerSecond_; }

private:
    double rowsPerSecond_;
    std::vector<double> power_;     // the average, as power
    double weight_ = 0;             // quiet rows in it, capped
    std::deque<float> levels_;      // recent rows' passband levels
    double quietRun_ = 0;           // quiet rows in a row
};

// The pth percentile (0..1) of some values.
float percentile(std::vector<float> values, double p);

} // namespace ear
} // namespace ragtime

#endif
