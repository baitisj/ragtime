#include "ragtime/voice/Chain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ragtime
{
namespace voice
{

namespace
{
// A sample at or over this hit the converter's full scale (32767/32768).
constexpr float CLIP_LEVEL = 32767.0f / 32768.0f;

constexpr uint64_t NO_LIMIT = std::numeric_limits<uint64_t>::max();
} // namespace

ChainSettings ChainSettings::clamped() const
{
    ChainSettings s = *this;
    s.inputGainDb = std::clamp(inputGainDb, -20.0, 40.0);
    // The high-pass has three positions; take the nearest.
    static const double corners[] = {80.0, 100.0, 150.0};
    double best = corners[0];
    for (double c : corners)
        if (std::fabs(highPassHz - c) < std::fabs(highPassHz - best))
            best = c;
    s.highPassHz = best;
    s.noiseAmount = std::clamp(noiseAmount, 0.0, 1.0);
    s.targetDb = std::clamp(targetDb, -36.0, -16.0);
    return s;
}

Chain::Chain(int inputRate, std::unique_ptr<Denoiser> denoiser)
    : inputRate_(inputRate)
    , denoiser_(std::move(denoiser))
    , highPass_(WORK_RATE, 100)
    , leveller_(BLOCK)
    , toOut_(WORK_RATE, OUT_RATE)
    , limiter_(OUT_RATE)
{
    if (inputRate_ != WORK_RATE)
        toWork_ = std::make_unique<Resampler>(inputRate_, WORK_RATE);
    settings_ = settings_.clamped();
    nextSettings_ = settings_;
    reset();
}

Chain::~Chain() = default;

void Chain::reset()
{
    if (toWork_)
        toWork_->reset();
    work_.clear();
    pending_.clear();
    inputLevel_.reset();
    highPass_.reset();
    if (denoiser_)
    {
        denoiser_->reset();
        dry_.assign((size_t)std::max(0, denoiser_->latency()), 0.0f);
    }
    else
        dry_.clear();
    dryPos_ = 0;
    leveller_.reset();
    toOut_.reset();
    limiter_.reset();
    beforeDelay_.clear();
    outputLevel_.reset();
    outBlockSum_ = 0;
    outBlockFill_ = 0;
    check_.reset();
    totalIn_ = 0;
    totalOut_ = 0;
    outLimit_ = NO_LIMIT;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_ = Readings();
        settingsChanged_ = true;
    }
    pickUpSettings();
    inputGain_ = std::pow(10.0, settings_.inputGainDb / 20.0);
    noiseMix_ = settings_.noiseIn ? settings_.noiseAmount : 0.0;
    call_ = CallMeters();
}

void Chain::setSettings(const ChainSettings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    nextSettings_ = settings.clamped();
    settingsChanged_ = true;
}

ChainSettings Chain::settings() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nextSettings_;
}

void Chain::pickUpSettings()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!settingsChanged_)
            return;
        settings_ = nextSettings_;
        settingsChanged_ = false;
    }
    if (settings_.highPassHz != highPass_.corner())
        highPass_.setCorner(settings_.highPassHz);
    leveller_.setTarget(settings_.targetDb);
    leveller_.setPeakMode(settings_.peakMode);
}

double Chain::latencySeconds() const
{
    double s = 0;
    if (toWork_)
        s += (double)toWork_->delayIn() / inputRate_;
    if (denoiser_)
        s += (double)denoiser_->latency() / WORK_RATE;
    s += (double)toOut_.delayIn() / WORK_RATE;
    s += (double)limiter_.delay() / OUT_RATE;
    return s;
}

void Chain::process(const float* in, size_t count, std::vector<float>& out)
{
    feed(in, count, out);
    totalIn_ += count;
    publish();
}

void Chain::feed(const float* in, size_t count, std::vector<float>& out)
{
    pickUpSettings();

    uint64_t clipped = 0;
    for (size_t i = 0; i < count; i++)
        if (std::fabs(in[i]) >= CLIP_LEVEL)
            clipped++;
    if (clipped)
    {
        call_.clipped = true;
        check_.addClipped(clipped);
    }

    const float* work = in;
    size_t workCount = count;
    if (toWork_)
    {
        work_.clear();
        toWork_->process(in, count, work_);
        work = work_.data();
        workCount = work_.size();
    }
    pending_.insert(pending_.end(), work, work + workCount);
    size_t used = 0;
    while (pending_.size() - used >= BLOCK)
    {
        runBlock(pending_.data() + used, out);
        used += BLOCK;
    }
    pending_.erase(pending_.begin(), pending_.begin() + (ptrdiff_t)used);
}

void Chain::runBlock(float* block, std::vector<float>& out)
{
    // The Gain knob, glided across the block.
    double gain = std::pow(10.0, settings_.inputGainDb / 20.0);
    double step = (gain - inputGain_) / (double)BLOCK;
    double g = inputGain_;
    double sum = 0;
    for (size_t i = 0; i < BLOCK; i++)
    {
        g += step;
        block[i] = (float)(block[i] * g);
        sum += (double)block[i] * block[i];
    }
    inputGain_ = gain;
    double meanSquare = sum / (double)BLOCK;
    inputLevel_.addBlock(meanSquare);
    call_.inLoudest = std::max(call_.inLoudest, powerDb(meanSquare));

    if (settings_.highPassIn)
        highPass_.process(block, BLOCK);

    if (denoiser_)
    {
        // The denoiser always runs, so switching it in or out keeps its
        // state warm and the delay the same; the amount mixes it with the
        // voice as it was, delayed to match.
        float dry[BLOCK];
        std::copy(block, block + BLOCK, dry);
        call_.voiceProbability = denoiser_->process(block);
        double mixTo = settings_.noiseIn ? settings_.noiseAmount : 0.0;
        double mixStep = (mixTo - noiseMix_) / (double)BLOCK;
        double mix = noiseMix_;
        for (size_t i = 0; i < BLOCK; i++)
        {
            float d = dry[i];
            if (!dry_.empty())
            {
                float delayed = dry_[dryPos_];
                dry_[dryPos_] = d;
                dryPos_ = (dryPos_ + 1) % dry_.size();
                d = delayed;
            }
            mix += mixStep;
            block[i] = (float)(mix * block[i] + (1.0 - mix) * d);
        }
        noiseMix_ = mixTo;
    }

    leveller_.processBlock(block);

    out8k_.clear();
    toOut_.process(block, BLOCK, out8k_);
    beforeDelay_.insert(beforeDelay_.end(), out8k_.begin(), out8k_.end());
    limited_.clear();
    limiter_.process(out8k_.data(), out8k_.size(), limited_);

    // Each limited sample lines up with the oldest one waiting before it.
    size_t n = limited_.size();
    if (outLimit_ != NO_LIMIT)
        n = (size_t)std::min<uint64_t>(n, outLimit_ > totalOut_ ? outLimit_ - totalOut_ : 0);
    check_.add(limited_.data(), beforeDelay_.data(), n);
    beforeDelay_.erase(beforeDelay_.begin(), beforeDelay_.begin() + (ptrdiff_t)limited_.size());
    for (size_t i = 0; i < n; i++)
    {
        outBlockSum_ += (double)limited_[i] * limited_[i];
        if (++outBlockFill_ == OUT_RATE / 100)
        {
            double ms = outBlockSum_ / (OUT_RATE / 100);
            outputLevel_.addBlock(ms);
            call_.outLoudest = std::max(call_.outLoudest, powerDb(ms));
            outBlockSum_ = 0;
            outBlockFill_ = 0;
        }
    }
    out.insert(out.end(), limited_.begin(), limited_.begin() + (ptrdiff_t)n);
    totalOut_ += n;
}

void Chain::startClip()
{
    pickUpSettings();
    check_.reset();
    check_.setTarget(settings_.targetDb);
    outLimit_ = NO_LIMIT;
}

void Chain::finish(std::vector<float>& out)
{
    // The output that covers everything fed so far: the input's length
    // at 8 kHz plus the chain's delay.
    double seconds = (double)totalIn_ / inputRate_ + latencySeconds();
    uint64_t end = (uint64_t)std::ceil(seconds * OUT_RATE);
    if (end > totalOut_)
    {
        outLimit_ = end;
        std::vector<float> zeros((size_t)std::max(1, inputRate_ / 100), 0.0f);
        // Silence until it is all out; the bound only guards against a
        // mistake in the sums above.
        for (int guard = 0; totalOut_ < end && guard < 1000; guard++)
        {
            feed(zeros.data(), zeros.size(), out);
            // The silence is in the stream now, as the microphone would be.
            totalIn_ += zeros.size();
        }
    }
    outLimit_ = NO_LIMIT;
    publish();
}

void Chain::publish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Readings& r = shared_;
    r.inputActiveDb = inputLevel_.levelDb();
    r.inputLoudestDb = std::max(r.inputLoudestDb, call_.inLoudest);
    r.clipped = r.clipped || call_.clipped;
    r.levellerGainDb = leveller_.gainDb();
    r.limiterReductionDb = std::max(r.limiterReductionDb, limiter_.takeReductionDb());
    r.speaking = leveller_.speaking();
    r.voiceProbability = call_.voiceProbability;
    r.outputActiveDb = outputLevel_.levelDb();
    r.outputLoudestDb = std::max(r.outputLoudestDb, call_.outLoudest);
    float probability = call_.voiceProbability;
    call_ = CallMeters();
    call_.voiceProbability = probability;
}

Readings Chain::takeReadings()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Readings r = shared_;
    shared_.inputLoudestDb = LEVEL_FLOOR_DBFS;
    shared_.clipped = false;
    shared_.limiterReductionDb = 0;
    shared_.outputLoudestDb = LEVEL_FLOOR_DBFS;
    return r;
}

void toPcm16(const float* in, size_t count, int16_t* out)
{
    for (size_t i = 0; i < count; i++)
    {
        double v = std::round((double)in[i] * 32768.0);
        out[i] = (int16_t)std::clamp(v, -32767.0, 32767.0);
    }
}

void fromPcm16(const int16_t* in, size_t count, float* out)
{
    for (size_t i = 0; i < count; i++)
        out[i] = (float)in[i] / 32768.0f;
}

} // namespace voice
} // namespace ragtime
