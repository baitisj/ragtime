// Spectrum: the Ear's rows. It takes the radio's receive audio at the
// sound card's own rate and turns it into rows of power, in dB below full
// scale, from 0 to TOP_HZ in BINS equal steps: one row for the waterfall
// every 1 / rowsPerSecond seconds.
//
// Each row is one FFT of FFT_HZ-wide bins (the length is the sound card's
// rate over FFT_HZ, rounded up to a power of two), under a Hann window,
// scaled so a full-scale sine reads 0 dB. The bins under each of the BINS
// steps are averaged as power. Rows are hop samples apart, overlapping
// when the hop is shorter than the FFT and skipping samples when longer.
//
// It runs on whatever thread feeds it; the Ear feeds it from PipeWire's.
#ifndef RAGTIME_EAR_SPECTRUM_H
#define RAGTIME_EAR_SPECTRUM_H

#include <complex>
#include <cstddef>
#include <vector>

namespace ragtime
{
namespace ear
{

class Spectrum
{
public:
    static constexpr double TOP_HZ = 4000.0;
    static constexpr int BINS = 512;
    static constexpr double FFT_HZ = 6.0;
    // The floor a silent bin reads, rather than minus infinity.
    static constexpr float FLOOR_DB = -160.0f;

    explicit Spectrum(int sampleRate = 48000, double rowsPerSecond = 10.0);

    // A new rate or speed starts afresh, dropping samples not yet in a row.
    void setRate(int sampleRate);
    void setRowsPerSecond(double rowsPerSecond);
    int rate() const { return rate_; }
    double rowsPerSecond() const { return rowsPerSecond_; }
    std::size_t fftLength() const { return n_; }

    // Takes n samples and hands back the rows they finish, oldest first.
    std::vector<std::vector<float>> push(const float* samples, std::size_t n);

    // The middle of bin i, in Hz.
    static double binHz(int i) { return (i + 0.5) * TOP_HZ / BINS; }
    // The bin a frequency falls in, clamped to the row.
    static int binOf(double hz);

private:
    void restart();
    std::vector<float> row();

    int rate_;
    double rowsPerSecond_;
    std::size_t n_ = 0;
    std::size_t hop_ = 0;
    std::vector<float> window_;
    double windowGain_ = 1.0;
    std::vector<float> buffer_;     // a ring of the last n_ samples
    std::size_t filled_ = 0;        // samples in buffer_, up to n_
    std::size_t next_ = 0;          // where the next sample goes
    std::size_t sinceRow_ = 0;      // samples since the last row
    std::vector<std::complex<double>> work_;
};

// An in-place radix-2 FFT; data.size() must be a power of two.
void fft(std::vector<std::complex<double>>& data);

} // namespace ear
} // namespace ragtime

#endif
