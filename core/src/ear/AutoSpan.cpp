#include "ragtime/ear/AutoSpan.h"

#include <algorithm>
#include <cmath>

#include "ragtime/ear/Spectrum.h"

namespace ragtime
{
namespace ear
{

bool fmMode(const std::string& mode)
{
    // FM, FMN, WFM, PKTFM, PKTFMN, C4FM.
    return mode.find("FM") != std::string::npos;
}

bool wideAudioMode(const std::string& mode)
{
    // And AM, AMN, SAM, PKTAM, AMS.
    return fmMode(mode) || mode.find("AM") != std::string::npos;
}

double AutoSpan::fit(double highHz)
{
    const double want = std::ceil((highHz + MARGIN_HZ) / STEP_HZ - 1e-9) * STEP_HZ;
    return std::max(Spectrum::MIN_TOP_HZ, std::min(Spectrum::MAX_TOP_HZ, want));
}

double AutoSpan::forMode(const std::string& mode)
{
    return wideAudioMode(mode) ? 8000.0 : Spectrum::DEFAULT_TOP_HZ;
}

double AutoSpan::update(uint64_t nowMs, double fixedHz, const std::optional<std::string>& mode,
                        const std::optional<Edges>& edges, bool reachesTop, double maxHz)
{
    const double cap = std::max(Spectrum::MIN_TOP_HZ, std::min(Spectrum::MAX_TOP_HZ, maxHz));
    auto clamp = [&](double hz) { return std::max(Spectrum::MIN_TOP_HZ, std::min(cap, hz)); };
    if (fixedHz > 0)
    {
        // Auto starts afresh when it is picked again.
        top_ = clamp(fixedHz);
        mode_.reset();
        narrower_.reset();
        return top_;
    }
    // A new mode (or the first): start from what it suggests.
    const bool newMode = mode && (!mode_ || *mode_ != *mode);
    if (top_ == 0 || newMode)
    {
        top_ = clamp(forMode(mode.value_or(std::string())));
        narrower_.reset();
        if (mode) mode_ = mode;
        return top_;
    }

    double want = top_;
    if (edges && edges->highHz)
    {
        want = clamp(fit(*edges->highHz));
    }
    else if ((edges && edges->lowHz) || (reachesTop && mode && wideAudioMode(*mode)))
    {
        // A clear shape whose high edge runs off the picture, or FM's audio
        // reaching the top: the next step.
        for (double step : CHOICES_HZ)
            if (step > top_ + 1)
            {
                want = clamp(step);
                break;
            }
    }

    if (want > top_ + 1)
    {
        top_ = want;
        narrower_.reset();
    }
    else if (want < top_ - 1)
    {
        if (!narrower_ || std::fabs(*narrower_ - want) > 1)
        {
            narrower_ = want;
            narrowerSinceMs_ = nowMs;
        }
        else if (nowMs >= narrowerSinceMs_ + NARROW_MS)
        {
            top_ = want;
            narrower_.reset();
        }
    }
    else
    {
        narrower_.reset();
    }
    return top_;
}

} // namespace ear
} // namespace ragtime
