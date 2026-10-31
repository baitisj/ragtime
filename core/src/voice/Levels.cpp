#include "ragtime/voice/Levels.h"

#include <algorithm>
#include <cmath>

namespace ragtime
{
namespace voice
{

double powerDb(double meanSquare)
{
    if (!(meanSquare > 0))
        return LEVEL_FLOOR_DBFS;
    return std::max(LEVEL_FLOOR_DBFS, 10.0 * std::log10(meanSquare));
}

// ---- ActiveLevel: ITU-T P.56 method B ------------------------------------

namespace
{
constexpr double P56_MARGIN_DB = 15.9;
constexpr double P56_TIME_S = 0.03;
constexpr double P56_HANGOVER_S = 0.2;

// Threshold j: 2^(j - 15), 1/32768 (one 16-bit step) up to full scale.
double threshold(int j)
{
    return std::ldexp(1.0, j - 15);
}
} // namespace

ActiveLevel::ActiveLevel(int sampleRate)
    : g_(std::exp(-1.0 / (sampleRate * P56_TIME_S)))
    , hangover_((uint64_t)std::lround(sampleRate * P56_HANGOVER_S))
{
    reset();
}

void ActiveLevel::reset()
{
    p_ = q_ = 0;
    sumSquares_ = 0;
    count_ = 0;
    active_.fill(0);
    // A new measurement starts outside any hangover.
    hang_.fill(hangover_);
}

void ActiveLevel::add(const float* samples, size_t count)
{
    double thresholds[THRESHOLDS];
    for (int j = 0; j < THRESHOLDS; j++)
        thresholds[j] = threshold(j);
    for (size_t i = 0; i < count; i++)
    {
        double x = samples[i];
        sumSquares_ += x * x;
        p_ = g_ * p_ + (1 - g_) * std::fabs(x);
        q_ = g_ * q_ + (1 - g_) * p_;
        for (int j = 0; j < THRESHOLDS; j++)
        {
            if (q_ >= thresholds[j])
            {
                active_[j]++;
                hang_[j] = 0;
            }
            else if (hang_[j] < hangover_)
            {
                active_[j]++;
                hang_[j]++;
            }
        }
    }
    count_ += count;
}

void ActiveLevel::result(double& level, double& activity) const
{
    level = LEVEL_FLOOR_DBFS;
    activity = 0;
    if (count_ == 0 || !(sumSquares_ > 0))
        return;
    // A_j: the RMS of the samples above threshold j; C_j: the threshold.
    // The active level is where A_j - C_j comes down to the margin.
    double prevA = 0, prevDelta = 0, prevAct = 0;
    bool havePrev = false;
    for (int j = 0; j < THRESHOLDS; j++)
    {
        if (active_[j] == 0)
            break;
        double a = 10.0 * std::log10(sumSquares_ / (double)active_[j]);
        double c = 20.0 * std::log10(threshold(j));
        double delta = a - c;
        double act = (double)active_[j] / (double)count_;
        if (delta <= P56_MARGIN_DB)
        {
            if (!havePrev)
            {
                level = a;
                activity = act;
            }
            else
            {
                // Between j - 1 and j, where the difference meets the margin.
                double t = (prevDelta - P56_MARGIN_DB) / (prevDelta - delta);
                level = prevA + t * (a - prevA);
                activity = prevAct + t * (act - prevAct);
            }
            level = std::max(LEVEL_FLOOR_DBFS, level);
            return;
        }
        prevA = a;
        prevDelta = delta;
        prevAct = act;
        havePrev = true;
    }
    // Never came down to the margin: as loud as it measures at the top.
    if (havePrev)
    {
        level = std::max(LEVEL_FLOOR_DBFS, prevA);
        activity = prevAct;
    }
}

double ActiveLevel::levelDb() const
{
    double level, activity;
    result(level, activity);
    return level;
}

double ActiveLevel::activity() const
{
    double level, activity;
    result(level, activity);
    return activity;
}

double ActiveLevel::longTermDb() const
{
    return count_ ? powerDb(sumSquares_ / (double)count_) : LEVEL_FLOOR_DBFS;
}

// ---- LiveLevel ----------------------------------------------------------

namespace
{
constexpr double BLOCK_S = 0.01;
constexpr int HANG_BLOCKS = 20;              // P.56's 200 ms
constexpr double NOISE_RISE_DB = 0.03;       // per block: 3 dB a second
constexpr double FIRST_GATE_OVER_NOISE = 12;
constexpr double FIRST_GATE_MIN_DB = -60;
constexpr double GATE_OVER_NOISE = 6;
constexpr double GATE_MIN_DB = -70;
} // namespace

LiveLevel::LiveLevel(double timeConstant)
    : alpha_(1.0 - std::exp(-BLOCK_S / timeConstant))
{
}

void LiveLevel::reset()
{
    power_ = 0;
    blocks_ = 0;
    noiseDb_ = 0;
    started_ = false;
    hang_ = 0;
    voiced_ = false;
}

bool LiveLevel::addBlock(double meanSquare)
{
    double db = powerDb(meanSquare);
    // The noise floor falls at once to a quieter block and creeps up.
    if (!started_ || db < noiseDb_)
        noiseDb_ = db;
    else
        noiseDb_ = std::min(db, noiseDb_ + NOISE_RISE_DB);
    started_ = true;

    double gate;
    if (blocks_ == 0)
        gate = std::max(FIRST_GATE_MIN_DB, noiseDb_ + FIRST_GATE_OVER_NOISE);
    else
        gate = std::max({GATE_MIN_DB, noiseDb_ + GATE_OVER_NOISE, levelDb() - P56_MARGIN_DB});

    bool speech;
    voiced_ = db > gate;
    if (voiced_)
    {
        speech = true;
        hang_ = HANG_BLOCKS;
    }
    else if (blocks_ > 0 && hang_ > 0)
    {
        speech = true;
        hang_--;
    }
    else
        speech = false;
    if (!speech)
        return false;

    blocks_++;
    // A plain mean until the time constant's worth is in, then exponential.
    double a = std::max(alpha_, 1.0 / (double)blocks_);
    power_ += a * (meanSquare - power_);
    return true;
}

double LiveLevel::levelDb() const
{
    return blocks_ ? powerDb(power_) : LEVEL_FLOOR_DBFS;
}

// ---- The Level check ----------------------------------------------------

const char* lampName(Lamp lamp)
{
    switch (lamp)
    {
    case Lamp::Green:
        return "green";
    case Lamp::Amber:
        return "amber";
    case Lamp::Red:
        return "red";
    }
    return "?";
}

Lamp activeLamp(double activeDb, double targetDb, bool speech)
{
    if (!speech)
        return Lamp::Red;
    double off = std::fabs(activeDb - targetDb);
    if (off <= 3.0)
        return Lamp::Green;
    if (off <= 6.0)
        return Lamp::Amber;
    return Lamp::Red;
}

Lamp loudestLamp(double loudestBeforeLimiterDb, uint64_t clippedSamples)
{
    if (clippedSamples > 0)
        return Lamp::Red;
    if (loudestBeforeLimiterDb <= LEVEL_MAX_DBFS)
        return Lamp::Green;
    if (loudestBeforeLimiterDb <= LEVEL_MAX_DBFS + 3.0)
        return Lamp::Amber;
    return Lamp::Red;
}

Lamp noiseLamp(double marginDb, bool speech)
{
    if (!speech)
        return Lamp::Red;
    if (marginDb >= 30.0)
        return Lamp::Green;
    if (marginDb >= 20.0)
        return Lamp::Amber;
    return Lamp::Red;
}

namespace
{
constexpr int WIN_10MS = LevelCheck::RATE / 100;  // 80
constexpr int HOP = WIN_10MS / 2;                // 40
constexpr int FRAME_20MS = LevelCheck::RATE / 50; // 160, sotto's frame
} // namespace

LevelCheck::LevelCheck(double targetDb)
    : target_(targetDb)
    , active_(RATE)
{
}

void LevelCheck::reset()
{
    active_.reset();
    tail_.clear();
    tailBefore_.clear();
    loudest_ = loudestBefore_ = 0;
    frames_.clear();
    frameSum_ = 0;
    frameFill_ = 0;
    clipped_ = 0;
    count_ = 0;
}

void LevelCheck::add(const float* out, const float* beforeLimiter, size_t count)
{
    active_.add(out, count);
    count_ += count;
    for (size_t i = 0; i < count; i++)
    {
        tail_.push_back(out[i]);
        tailBefore_.push_back(beforeLimiter[i]);
        if ((int)tail_.size() == WIN_10MS)
        {
            double s = 0, sb = 0;
            for (int k = 0; k < WIN_10MS; k++)
            {
                s += (double)tail_[k] * tail_[k];
                sb += (double)tailBefore_[k] * tailBefore_[k];
            }
            loudest_ = std::max(loudest_, s / WIN_10MS);
            loudestBefore_ = std::max(loudestBefore_, sb / WIN_10MS);
            tail_.erase(tail_.begin(), tail_.begin() + HOP);
            tailBefore_.erase(tailBefore_.begin(), tailBefore_.begin() + HOP);
        }
        frameSum_ += (double)out[i] * out[i];
        if (++frameFill_ == FRAME_20MS)
        {
            frames_.push_back(frameSum_ / FRAME_20MS);
            frameSum_ = 0;
            frameFill_ = 0;
        }
    }
}

LevelReport LevelCheck::report() const
{
    LevelReport r;
    r.seconds = (double)count_ / RATE;
    r.activeDb = active_.levelDb();
    r.activity = active_.activity();
    r.loudestDb = powerDb(loudest_);
    r.loudestBeforeLimiterDb = powerDb(loudestBefore_);
    r.clippedSamples = clipped_;
    // Speech: something active that sotto wouldn't send as silence.
    r.speech = r.activity > 0 && r.activeDb > LEVEL_SILENT_DBFS;

    if (!frames_.empty())
    {
        std::vector<double> sorted = frames_;
        std::sort(sorted.begin(), sorted.end());
        size_t tenth = std::max<size_t>(1, sorted.size() / 10);
        double sum = 0;
        for (size_t i = 0; i < tenth; i++)
            sum += sorted[i];
        r.noiseDb = powerDb(sum / (double)tenth);
    }
    r.marginDb = r.activeDb - r.noiseDb;

    r.active = activeLamp(r.activeDb, target_, r.speech);
    r.loudest = loudestLamp(r.loudestBeforeLimiterDb, r.clippedSamples);
    r.noise = noiseLamp(r.marginDb, r.speech);
    return r;
}

} // namespace voice
} // namespace ragtime
