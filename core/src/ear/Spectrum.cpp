#include "ragtime/ear/Spectrum.h"

#include <algorithm>
#include <cmath>

namespace ragtime
{
namespace ear
{

namespace
{

constexpr double PI = 3.14159265358979323846;

std::size_t powerOfTwoAtLeast(double n)
{
    std::size_t p = 1;
    while ((double)p < n) p <<= 1;
    return p;
}

} // namespace

void fft(std::vector<std::complex<double>>& data)
{
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; i++)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * PI / (double)len;
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; k++)
            {
                const std::complex<double> a = data[start + k];
                const std::complex<double> b = data[start + k + len / 2] * w;
                data[start + k] = a + b;
                data[start + k + len / 2] = a - b;
                w *= step;
            }
        }
    }
}

Spectrum::Spectrum(int sampleRate, double rowsPerSecond)
    : rate_(sampleRate > 0 ? sampleRate : 48000)
    , rowsPerSecond_(rowsPerSecond > 0 ? rowsPerSecond : 10.0)
{
    restart();
}

void Spectrum::setRate(int sampleRate)
{
    if (sampleRate <= 0 || sampleRate == rate_) return;
    rate_ = sampleRate;
    restart();
}

void Spectrum::setRowsPerSecond(double rowsPerSecond)
{
    if (rowsPerSecond <= 0 || rowsPerSecond == rowsPerSecond_) return;
    rowsPerSecond_ = rowsPerSecond;
    restart();
}

void Spectrum::setTopHz(double topHz)
{
    topHz = std::max(MIN_TOP_HZ, std::min(MAX_TOP_HZ, topHz));
    if (topHz == topHz_) return;
    topHz_ = topHz;
    restart();
}

int Spectrum::binOf(double hz, double topHz)
{
    int i = (int)std::floor(hz / topHz * BINS);
    return std::max(0, std::min(BINS - 1, i));
}

float Spectrum::quietLevel(const std::vector<float>& row, double topHz, double lowHz, double highHz)
{
    if (row.empty()) return 0.0f;
    std::size_t from = (std::size_t)binOf(lowHz, topHz), to = (std::size_t)binOf(highHz, topHz) + 1;
    if (to > row.size() || to < from + 4)
    {
        from = 0;
        to = row.size();
    }
    std::vector<float> part(row.begin() + (long)from, row.begin() + (long)to);
    const std::size_t k = part.size() * 3 / 10;
    std::nth_element(part.begin(), part.begin() + (long)k, part.end());
    return part[k];
}

void Spectrum::restart()
{
    n_ = powerOfTwoAtLeast(rate_ / FFT_HZ);
    hop_ = std::max<std::size_t>(1, (std::size_t)std::lround(rate_ / rowsPerSecond_));
    window_.resize(n_);
    double sum = 0;
    for (std::size_t i = 0; i < n_; i++)
    {
        window_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * PI * (double)i / (double)n_));
        sum += window_[i];
    }
    windowGain_ = sum;
    buffer_.assign(n_, 0.0f);
    filled_ = 0;
    next_ = 0;
    sinceRow_ = 0;
    work_.resize(n_);
}

std::vector<std::vector<float>> Spectrum::push(const float* samples, std::size_t n)
{
    std::vector<std::vector<float>> rows;
    for (std::size_t i = 0; i < n; i++)
    {
        // A ring of the last n_ samples; next_ is the oldest once full.
        buffer_[next_] = samples[i];
        next_ = (next_ + 1) % n_;
        if (filled_ < n_) filled_++;
        if (++sinceRow_ >= hop_ && filled_ == n_)
        {
            rows.push_back(row());
            sinceRow_ = 0;
        }
    }
    return rows;
}

std::vector<float> Spectrum::row()
{
    for (std::size_t i = 0; i < n_; i++)
        work_[i] = std::complex<double>(buffer_[(next_ + i) % n_] * window_[i], 0.0);
    fft(work_);

    // A full-scale sine of amplitude 1 puts |X| = gain / 2 in its bin, so
    // power (2 |X| / gain)^2 reads 1, 0 dB.
    const double scale = 4.0 / (windowGain_ * windowGain_);
    const double binHz = (double)rate_ / (double)n_;
    std::vector<float> out(BINS);
    for (int b = 0; b < BINS; b++)
    {
        const double lo = b * topHz_ / BINS, hi = (b + 1) * topHz_ / BINS;
        std::size_t first = (std::size_t)std::ceil(lo / binHz);
        std::size_t last = (std::size_t)std::ceil(hi / binHz);
        last = std::min(last, n_ / 2);
        double power = 0;
        int count = 0;
        for (std::size_t k = first; k < last; k++)
        {
            power += std::norm(work_[k]) * scale;
            count++;
        }
        if (count == 0)
        {
            // A step narrower than an FFT bin: the bin it sits in.
            std::size_t k = std::min((std::size_t)std::floor((lo + hi) / 2 / binHz), n_ / 2);
            power = std::norm(work_[k]) * scale;
            count = 1;
        }
        power /= count;
        out[b] = power > 0 ? std::max(FLOOR_DB, (float)(10.0 * std::log10(power))) : FLOOR_DB;
    }
    return out;
}

} // namespace ear
} // namespace ragtime
