#include "ragtime/voice/Dsp.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ragtime
{
namespace voice
{

namespace
{
constexpr double PI = 3.14159265358979323846;

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}
} // namespace

// ---- HighPass ------------------------------------------------------------

HighPass::HighPass(double sampleRate, double cornerHz)
    : rate_(sampleRate)
{
    setCorner(cornerHz);
}

void HighPass::setCorner(double cornerHz)
{
    corner_ = std::clamp(cornerHz, 1.0, rate_ * 0.45);
    // The two poles pairs of a fourth-order Butterworth, as RBJ high-pass
    // biquads.
    static const double Q[2] = {0.54119610014619698, 1.3065629648763766};
    double w0 = 2 * PI * corner_ / rate_;
    double cw = std::cos(w0), sw = std::sin(w0);
    for (int i = 0; i < 2; i++)
    {
        double alpha = sw / (2 * Q[i]);
        double a0 = 1 + alpha;
        Biquad& b = stage_[i];
        b.b0 = (1 + cw) / 2 / a0;
        b.b1 = -(1 + cw) / a0;
        b.b2 = (1 + cw) / 2 / a0;
        b.a1 = -2 * cw / a0;
        b.a2 = (1 - alpha) / a0;
    }
}

void HighPass::reset()
{
    for (Biquad& b : stage_)
        b.z1 = b.z2 = 0;
}

void HighPass::process(float* samples, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        double x = samples[i];
        for (Biquad& b : stage_)
        {
            // Transposed direct form II.
            double y = b.b0 * x + b.z1;
            b.z1 = b.b1 * x - b.a1 * y + b.z2;
            b.z2 = b.b2 * x - b.a2 * y;
            x = y;
        }
        samples[i] = (float)x;
    }
}

// ---- Resampler -----------------------------------------------------------

namespace
{
// The zeroth-order modified Bessel function, for the Kaiser window.
double besselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++)
    {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}
} // namespace

Resampler::Resampler(int inRate, int outRate)
    : inRate_(inRate)
    , outRate_(outRate)
{
    int g = std::gcd(inRate, outRate);
    up_ = outRate / g;
    down_ = inRate / g;

    // Pass band to 90% of the lower Nyquist frequency, stop band from it,
    // 80 dB down (Kaiser's formulas).
    const double atten = 80.0;
    double nyq = 0.5 * std::min(inRate, outRate);
    double fsUp = (double)inRate * up_;
    double transition = 0.1 * nyq;
    double cutoff = 0.95 * nyq / fsUp;  // cycles per upsampled sample
    double beta = 0.1102 * (atten - 8.7);
    int length = (int)std::ceil((atten - 8.0) / (2.285 * 2 * PI * transition / fsUp)) + 1;
    taps_ = std::max(1, (length + up_ - 1) / up_);
    int total = taps_ * up_;

    h_.assign((size_t)total, 0.0f);
    double centre = (total - 1) / 2.0;
    double i0beta = besselI0(beta);
    std::vector<double> proto((size_t)total);
    for (int n = 0; n < total; n++)
    {
        double t = n - centre;
        double sinc = t == 0 ? 2 * cutoff : std::sin(2 * PI * cutoff * t) / (PI * t);
        double r = 2.0 * n / (total - 1) - 1.0;
        double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0beta;
        proto[(size_t)n] = sinc * w * up_;
    }
    // Phase-major: phase p's taps are proto[p], proto[p + L], ...
    for (int p = 0; p < up_; p++)
        for (int k = 0; k < taps_; k++)
            h_[(size_t)p * taps_ + k] = (float)proto[(size_t)(p + k * up_)];
    delayIn_ = (size_t)std::ceil(centre / up_);
    reset();
}

void Resampler::reset()
{
    hist_.clear();
    histStart_ = 0;
    inCount_ = 0;
    outCount_ = 0;
}

void Resampler::process(const float* in, size_t count, std::vector<float>& out)
{
    hist_.insert(hist_.end(), in, in + count);
    inCount_ += count;
    for (;;)
    {
        uint64_t t = outCount_ * (uint64_t)down_;
        uint64_t base = t / (uint64_t)up_;
        if (base >= inCount_)
            break;
        int phase = (int)(t % (uint64_t)up_);
        const float* h = h_.data() + (size_t)phase * taps_;
        double y = 0;
        // x[base - k] for k = 0 .. taps - 1; before the first sample is 0.
        int kmax = (int)std::min<uint64_t>((uint64_t)taps_ - 1, base);
        const float* x = hist_.data() + (base - histStart_);
        for (int k = 0; k <= kmax; k++)
            y += (double)h[k] * x[-k];
        out.push_back((float)y);
        outCount_++;
    }
    // Keep what the next output still needs.
    uint64_t next = outCount_ * (uint64_t)down_ / (uint64_t)up_;
    uint64_t keepFrom = next + 1 > (uint64_t)taps_ ? next + 1 - (uint64_t)taps_ : 0;
    if (keepFrom > histStart_)
    {
        size_t drop = (size_t)std::min<uint64_t>(keepFrom - histStart_, hist_.size());
        hist_.erase(hist_.begin(), hist_.begin() + (ptrdiff_t)drop);
        histStart_ += drop;
    }
}

// ---- Limiter -------------------------------------------------------------

Limiter::Limiter(int sampleRate, double rmsCeilingDb, double peakCeilingDb)
    : window_(sampleRate / 100)
    , attack_(sampleRate / 200)
    , rmsCeiling_((float)dbToGain(rmsCeilingDb))
    , peakCeiling_((float)dbToGain(peakCeilingDb))
    , release_((float)(1.0 - std::exp(-1.0 / (0.050 * sampleRate))))
{
    window_ = std::clamp(window_, 1, RING / 4);
    attack_ = std::clamp(attack_, 1, RING / 4);
    delay_ = (size_t)(window_ + attack_ - 2);
    reset();
}

void Limiter::reset()
{
    std::fill(std::begin(x_), std::end(x_), 0.0f);
    std::fill(std::begin(rmsReq_), std::end(rmsReq_), 1.0f);
    std::fill(std::begin(q_), std::end(q_), 1.0f);
    std::fill(std::begin(m_), std::end(m_), 1.0f);
    sumSquares_ = 0;
    mSum_ = attack_;  // m before the first sample counts as 1
    n_ = 0;
    gain_ = 1.0f;
    minGain_ = 1.0f;
}

void Limiter::process(const float* in, size_t count, std::vector<float>& out)
{
    const uint64_t R = (uint64_t)window_, K = (uint64_t)attack_;
    for (size_t i = 0; i < count; i++)
    {
        const uint64_t n = n_++;
        float x = in[i];
        x_[n % RING] = x;
        sumSquares_ += (double)x * x;
        if (n >= R)
        {
            float old = x_[(n - R) % RING];
            sumSquares_ -= (double)old * old;
        }
        if (sumSquares_ < 0)
            sumSquares_ = 0;
        // The gain the 10 ms ending here needs, applied across all of it.
        double rms = std::sqrt(sumSquares_ / (double)R);
        rmsReq_[n % RING] = rms > rmsCeiling_ ? (float)(rmsCeiling_ / rms) : 1.0f;

        if (n + 1 < R)
            continue;
        // q(s): what sample s needs, from its own peak and every 10 ms
        // window that holds it.
        const uint64_t s1 = n + 1 - R;
        float ax = std::fabs(x_[s1 % RING]);
        float q = ax > peakCeiling_ ? peakCeiling_ / ax : 1.0f;
        for (uint64_t t = s1; t <= n; t++)
            q = std::min(q, rmsReq_[t % RING]);
        q_[s1 % RING] = q;

        if (s1 + 1 < K)
            continue;
        // m(s): the least q over the next K samples; then the mean of the
        // last K m's, which falls over K samples and is never above q(s).
        const uint64_t s2 = s1 + 1 - K;
        float m = 1.0f;
        for (uint64_t u = s2; u <= s1; u++)
            m = std::min(m, q_[u % RING]);
        float leaving = s2 >= K ? m_[(s2 - K) % RING] : 1.0f;
        m_[s2 % RING] = m;
        mSum_ += (double)m - leaving;
        float h = (float)std::min(1.0, mSum_ / (double)K);

        gain_ = std::min(h, gain_ + (1.0f - gain_) * release_);
        minGain_ = std::min(minGain_, gain_);
        out.push_back(x_[s2 % RING] * gain_);
    }
}

double Limiter::takeReductionDb()
{
    double db = minGain_ < 1.0f ? -20.0 * std::log10(std::max(minGain_, 1e-6f)) : 0.0;
    minGain_ = gain_;
    return db;
}

// ---- Leveller ------------------------------------------------------------

Leveller::Leveller(size_t blockSize)
    : block_(blockSize)
    , target_(LEVEL_TARGET_DBFS)
{
}

void Leveller::reset()
{
    level_.reset();
    gainDb_ = 0;
    levelGainDb_ = 0;
    peakCutDb_ = 0;
    appliedGain_ = 1.0;
    speechBlocks_ = 0;
    speaking_ = false;
    lastPeakCut_ = 0;
}

void Leveller::processBlock(float* block)
{
    double sum = 0;
    for (size_t i = 0; i < block_; i++)
        sum += (double)block[i] * block[i];
    double meanSquare = sum / (double)block_;

    speaking_ = level_.addBlock(meanSquare);
    lastPeakCut_ = 0;
    // The gain moves on speech itself, not in the hangover after it.
    if (level_.voiced())
    {
        speechBlocks_++;
        double want = std::clamp(target_ - level_.levelDb(), MIN_GAIN_DB, MAX_GAIN_DB);
        bool fast = (double)speechBlocks_ * 0.01 <= FAST_SECONDS;
        double up = (fast ? FAST_DB_PER_S : UP_DB_PER_S) * 0.01;
        double down = (fast ? FAST_DB_PER_S : DOWN_DB_PER_S) * 0.01;
        if (want > levelGainDb_)
            levelGainDb_ = std::min(want, levelGainDb_ + up);
        else
            levelGainDb_ = std::max(want, levelGainDb_ - down);
        // What peak mode took off comes back slowly, and only while
        // someone talks.
        peakCutDb_ = std::max(0.0, peakCutDb_ - PEAK_RECOVER_DB_PER_S * 0.01);
    }
    if (!peakMode_)
        peakCutDb_ = 0;
    else
    {
        double out = powerDb(meanSquare) + levelGainDb_ - peakCutDb_;
        if (out > PEAK_MODE_DBFS)
        {
            lastPeakCut_ = out - PEAK_MODE_DBFS;
            peakCutDb_ += lastPeakCut_;
        }
    }
    gainDb_ = std::max(MIN_GAIN_DB, levelGainDb_ - peakCutDb_);

    // Glide to the new gain across the block, or within 1 ms for a peak
    // cut, which belongs to this very block.
    double target = dbToGain(gainDb_);
    size_t ramp = lastPeakCut_ > 0 ? std::max<size_t>(1, block_ / 10) : block_;
    double step = (target - appliedGain_) / (double)ramp;
    double g = appliedGain_;
    for (size_t i = 0; i < block_; i++)
    {
        if (i < ramp)
            g += step;
        block[i] = (float)(block[i] * g);
    }
    appliedGain_ = target;
}

} // namespace voice
} // namespace ragtime
