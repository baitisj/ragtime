#include "ragtime/ear/Edges.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "ragtime/ear/Spectrum.h"

namespace ragtime
{
namespace ear
{

namespace
{

// Bins either side averaged before looking for the edges.
constexpr int SMOOTH = 2;
// The stopband is read this far outside the passband's ends.
constexpr double SKIRT_HZ = 100.0;

} // namespace

float percentile(std::vector<float> values, double p)
{
    if (values.empty()) return 0.0f;
    const std::size_t k = std::min(values.size() - 1, (std::size_t)(p * (double)(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + (long)k, values.end());
    return values[k];
}

EdgeFinder::EdgeFinder(double rowsPerSecond)
    : rowsPerSecond_(rowsPerSecond > 0 ? rowsPerSecond : 10.0)
    , topHz_(Spectrum::DEFAULT_TOP_HZ)
{
}

void EdgeFinder::setTopHz(double topHz)
{
    if (topHz <= 0 || topHz == topHz_) return;
    topHz_ = topHz;
    clear();
}

double EdgeFinder::binHz(int i) const
{
    return Spectrum::binHz(i, topHz_);
}

void EdgeFinder::setRowsPerSecond(double rowsPerSecond)
{
    if (rowsPerSecond <= 0 || rowsPerSecond == rowsPerSecond_) return;
    rowsPerSecond_ = rowsPerSecond;
    clear();
}

void EdgeFinder::clear()
{
    power_.clear();
    weight_ = 0;
    levels_.clear();
    quietRun_ = 0;
    holdRows_ = 0;
}

void EdgeFinder::addRow(const std::vector<float>& row, bool transmitting)
{
    if ((int)row.size() != Spectrum::BINS) return;
    if (transmitting)
    {
        quietRun_ = 0;
        return;
    }
    if (holdRows_ > 0)
    {
        // The rig's AGC settling after a write: not the band.
        holdRows_ = std::max(0.0, holdRows_ - 1);
        quietRun_ = 0;
        return;
    }

    // A row lifted over the quieter rows of the last while has a signal in it.
    const float level = percentile(row, 0.9);
    bool quiet = true;
    if ((double)levels_.size() >= rowsPerSecond_)
    {
        const float floor = percentile(std::vector<float>(levels_.begin(), levels_.end()), 0.2);
        quiet = level <= floor + BURST_DB;
    }
    levels_.push_back(level);
    while ((double)levels_.size() > HISTORY_S * rowsPerSecond_) levels_.pop_front();
    if (!quiet)
    {
        quietRun_ = 0;
        return;
    }
    quietRun_++;

    if (power_.empty()) power_.assign(Spectrum::BINS, 0.0);
    weight_ = std::min(weight_ + 1.0, AVERAGE_S * rowsPerSecond_);
    const double a = 1.0 / weight_;
    for (int i = 0; i < Spectrum::BINS; i++) power_[i] += (std::pow(10.0, row[i] / 10.0) - power_[i]) * a;
}

void EdgeFinder::restart(double holdSeconds)
{
    power_.clear();
    weight_ = 0;
    holdRows_ = std::max(holdRows_, holdSeconds * rowsPerSecond_);
}

void EdgeFinder::setBridge(double lowHz, double highHz)
{
    bridgeLowHz_ = lowHz;
    bridgeHighHz_ = highHz;
}

std::optional<Dip> EdgeFinder::dip(double lowHz, double highHz) const
{
    std::vector<float> db;
    std::vector<bool> heard;
    double passband = 0;
    if (!shape(db, heard, passband)) return std::nullopt;
    int best = -1;
    for (int i = 0; i < Spectrum::BINS; i++)
    {
        const double hz = binHz(i);
        if (hz < lowHz || hz > highHz || !heard[i]) continue;
        if (best < 0 || db[i] < db[best]) best = i;
    }
    if (best < 0 || passband - db[best] < DIP_DB) return std::nullopt;
    Dip d;
    d.depthDb = passband - db[best];
    d.hz = binHz(best);
    // Between bins: the parabola through the deepest and its neighbours.
    if (best > 0 && best < Spectrum::BINS - 1 && heard[best - 1] && heard[best + 1])
    {
        const double a = db[best - 1], b = db[best], c = db[best + 1];
        const double bend = a - 2 * b + c;
        if (bend > 0) d.hz += 0.5 * (a - c) / bend * topHz_ / Spectrum::BINS;
    }
    return d;
}

double EdgeFinder::quietSeconds() const
{
    return weight_ / rowsPerSecond_;
}

std::vector<float> EdgeFinder::average() const
{
    std::vector<float> out;
    for (double p : power_) out.push_back((float)(10.0 * std::log10(std::max(p, 1e-16))));
    return out;
}

bool EdgeFinder::reachesTop() const
{
    if (power_.empty() || quietSeconds() < NEEDED_S) return false;
    std::vector<float> db;
    for (double p : power_)
        if (p > 1e-15) db.push_back((float)(10.0 * std::log10(p)));
    if (db.size() < 40) return false;
    // The passband's level, as edges() finds it.
    const float top = percentile(db, 0.9);
    std::vector<float> near;
    for (float v : db)
        if (v >= top - 10.0f) near.push_back(v);
    const double passband = percentile(near, 0.5);
    // The highest twentieth of the bins heard, as power.
    double sum = 0;
    const std::size_t n = db.size() / 20;
    for (std::size_t i = db.size() - n; i < db.size(); i++) sum += std::pow(10.0, db[i] / 10.0);
    return 10.0 * std::log10(sum / (double)n) >= passband - DROP_DB;
}

bool EdgeFinder::shape(std::vector<float>& db, std::vector<bool>& heard, double& passband) const
{
    if (power_.empty() || quietSeconds() < NEEDED_S) return false;

    // Smoothed a little, as power, then in dB. Bins with no audio at all
    // (past half the sound card's rate) are left out.
    const int n = Spectrum::BINS;
    db.assign(n, 0.0f);
    heard.assign(n, false);
    for (int i = 0; i < n; i++)
    {
        double sum = 0;
        int count = 0;
        for (int j = std::max(0, i - SMOOTH); j <= std::min(n - 1, i + SMOOTH); j++)
        {
            sum += power_[j];
            count++;
        }
        db[i] = (float)(10.0 * std::log10(std::max(sum / count, 1e-16)));
        heard[i] = power_[i] > 1e-15;
    }
    std::vector<float> levels;
    for (int i = 0; i < n; i++)
        if (heard[i]) levels.push_back(db[i]);
    if (levels.size() < 10) return false;

    // The passband's level: the median of the bins near the top.
    const float top = percentile(levels, 0.9);
    std::vector<float> near;
    for (float v : levels)
        if (v >= top - 10.0f) near.push_back(v);
    passband = percentile(near, 0.5);
    return true;
}

std::optional<Edges> EdgeFinder::edges() const
{
    std::vector<float> db;
    std::vector<bool> heard;
    double passband = 0;
    if (!shape(db, heard, passband)) return std::nullopt;
    const int n = Spectrum::BINS;
    const double threshold = passband - DROP_DB;

    const double binWidth = topHz_ / Spectrum::BINS;
    // The widest run over the threshold, short dips allowed, and any dip
    // inside the notch's bridge.
    const int gap = (int)std::lround(GAP_HZ / binWidth);
    auto bridged = [&](int from, int to) {
        return bridgeHighHz_ > bridgeLowHz_ && binHz(from) >= bridgeLowHz_ && binHz(to) <= bridgeHighHz_;
    };
    int bestStart = -1, bestEnd = -1;
    int start = -1, end = -1;
    for (int i = 0; i <= n; i++)
    {
        const bool over = i < n && heard[i] && db[i] >= threshold;
        if (over)
        {
            if (start < 0 || (i - end - 1 > gap && !bridged(end + 1, i - 1)))
            {
                if (start >= 0 && end - start > bestEnd - bestStart)
                {
                    bestStart = start;
                    bestEnd = end;
                }
                start = i;
            }
            end = i;
        }
    }
    if (start >= 0 && end - start > bestEnd - bestStart)
    {
        bestStart = start;
        bestEnd = end;
    }
    if (bestStart < 0) return std::nullopt;

    // The stopband: everything clear of the run's skirts.
    const int skirt = (int)std::lround(SKIRT_HZ / binWidth);
    std::vector<float> outside;
    for (int i = 0; i < n; i++)
        if (heard[i] && (i < bestStart - skirt || i > bestEnd + skirt)) outside.push_back(db[i]);
    if (outside.empty()) return std::nullopt;
    const double stopband = percentile(outside, 0.5);
    if (passband - stopband < STOP_DB) return std::nullopt;

    Edges e;
    e.passbandDb = passband;
    e.stopbandDb = stopband;
    e.quietSeconds = quietSeconds();
    // Between the last bin under the threshold and the first over it.
    auto cross = [&](int under, int over) {
        const double span = db[over] - db[under];
        const double t = span > 0 ? (threshold - db[under]) / span : 0.5;
        return binHz(under) + (over - under) * binWidth * std::max(0.0, std::min(1.0, t));
    };
    if (bestStart > 0 && heard[bestStart - 1] && binHz(bestStart) > END_HZ)
        e.lowHz = cross(bestStart - 1, bestStart);
    if (bestEnd < n - 1 && heard[bestEnd + 1] && binHz(bestEnd) < topHz_ - END_HZ)
        e.highHz = cross(bestEnd + 1, bestEnd);
    return e;
}

} // namespace ear
} // namespace ragtime
