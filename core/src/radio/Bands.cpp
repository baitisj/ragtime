#include "ragtime/radio/Bands.h"

#include <cstdio>
#include <string>

namespace ragtime
{
namespace radio
{

const std::vector<Span>& amateurBands()
{
    // 47 CFR 97.301, the US allocations. 1.25 m is two pieces.
    static const std::vector<Span> bands = {
        {135700, 137800, "2200 m"},
        {472000, 479000, "630 m"},
        {1800000, 2000000, "160 m"},
        {3500000, 4000000, "80 m"},
        {5330500, 5406400, "60 m"},
        {7000000, 7300000, "40 m"},
        {10100000, 10150000, "30 m"},
        {14000000, 14350000, "20 m"},
        {18068000, 18168000, "17 m"},
        {21000000, 21450000, "15 m"},
        {24890000, 24990000, "12 m"},
        {28000000, 29700000, "10 m"},
        {50000000, 54000000, "6 m"},
        {144000000, 148000000, "2 m"},
        {219000000, 220000000, "1.25 m"},
        {222000000, 225000000, "1.25 m"},
        {420000000, 450000000, "70 cm"},
        {902000000, 928000000, "33 cm"},
        {1240000000, 1300000000, "23 cm"},
    };
    return bands;
}

std::string bandOf(uint64_t hz)
{
    for (const Span& s : amateurBands())
    {
        if (hz >= s.lowHz && hz <= s.highHz) return s.band;
    }
    return std::string();
}

size_t bandOrder(const std::string& band)
{
    const std::vector<Span>& bands = amateurBands();
    for (size_t i = 0; i < bands.size(); i++)
    {
        if (band == bands[i].band) return i;
    }
    return bands.size();
}

const std::vector<Span>& usDataSegments()
{
    // 47 CFR 97.305(c): where RTTY and data are authorized.
    static const std::vector<Span> segments = {
        {1800000, 2000000, "160 m"},
        {3500000, 3600000, "80 m"},
        {5351500, 5366500, "60 m"},
        {7000000, 7125000, "40 m"},
        {10100000, 10150000, "30 m"},
        {14000000, 14150000, "20 m"},
        {18068000, 18110000, "17 m"},
        {21000000, 21200000, "15 m"},
        {24890000, 24930000, "12 m"},
        {28000000, 28300000, "10 m"},
        {50100000, 54000000, "6 m"},
        {144100000, 148000000, "2 m"},
        {222000000, 225000000, "1.25 m"},
        {420000000, 450000000, "70 cm"},
        {902000000, 928000000, "33 cm"},
        {1240000000, 1300000000, "23 cm"},
    };
    return segments;
}

std::string formatMHz(uint64_t hz)
{
    char text[40];
    std::snprintf(text, sizeof text, "%llu.%06llu", (unsigned long long)(hz / 1000000),
                  (unsigned long long)(hz % 1000000));
    std::string out = text;
    // Down to three decimals, never fewer.
    while (out.size() > out.find('.') + 4 && out.back() == '0') out.pop_back();
    return out + " MHz";
}

const std::vector<uint64_t>& us60mChannelCentres()
{
    static const std::vector<uint64_t> centres = {5332000, 5348000, 5373000, 5405000};
    return centres;
}

const Span& us60mSegment()
{
    static const Span segment = {5351500, 5366500, "60 m"};
    return segment;
}

namespace
{

// The 60 m channel whose dial setting this is, to the hertz; 0 if none.
uint64_t on60mChannel(uint64_t dialHz)
{
    for (uint64_t centre : us60mChannelCentres())
    {
        if (dialHz == centre - US_60M_DIAL_BELOW_HZ) return centre;
    }
    return 0;
}

// The segment the dial is inside with room for the burst, or none.
const Span* insideSegment(uint64_t dialHz)
{
    for (const Span& s : usDataSegments())
    {
        if (dialHz >= s.lowHz + SEGMENT_MARGIN_HZ && dialHz + SEGMENT_MARGIN_HZ <= s.highHz) return &s;
    }
    return nullptr;
}

// "14.000-14.150 MHz"
std::string rangeText(const Span& s)
{
    std::string low = formatMHz(s.lowHz);
    return low.substr(0, low.size() - 4) + "-" + formatMHz(s.highHz);
}

} // namespace

std::string usDataRefusal(uint64_t dialHz)
{
    if (on60mChannel(dialHz) != 0 || insideSegment(dialHz) != nullptr) return std::string();
    std::string why = formatMHz(dialHz);
    if (bandOf(dialHz) == "60 m")
    {
        // Every channel's dial setting, in order, then the band segment.
        why += " is on 60 m, where data goes out only centred on a channel (47 CFR 97.303(h)): the dial at ";
        const std::vector<uint64_t>& centres = us60mChannelCentres();
        for (size_t i = 0; i < centres.size(); i++)
        {
            std::string dial = formatMHz(centres[i] - US_60M_DIAL_BELOW_HZ);
            if (i + 1 < centres.size()) dial = dial.substr(0, dial.size() - 4);
            why += (i == 0 ? "" : i + 1 == centres.size() ? " or " : ", ") + dial;
        }
        const Span& seg = us60mSegment();
        Span room = {seg.lowHz + SEGMENT_MARGIN_HZ, seg.highHz - SEGMENT_MARGIN_HZ, seg.band};
        why += " in USB, 1.5 kHz below the channel's centre, exactly; or in the band segment "
               + rangeText(seg) + ", with the dial at " + rangeText(room) + ".";
        return why;
    }
    const Span* nearest = nullptr;
    uint64_t nearestHz = UINT64_MAX;
    for (const Span& s : usDataSegments())
    {
        uint64_t away = dialHz < s.lowHz ? s.lowHz - dialHz : dialHz > s.highHz ? dialHz - s.highHz : 0;
        if (away < nearestHz)
        {
            nearest = &s;
            nearestHz = away;
        }
    }
    why += " is outside the US data segments (47 CFR 97.305).";
    if (nearest != nullptr)
        why += " The nearest is " + rangeText(*nearest) + " (" + nearest->band + "), with 3 kHz to spare at each edge.";
    return why;
}

std::string usDataPlace(uint64_t dialHz)
{
    if (uint64_t centre = on60mChannel(dialHz))
        return "Centred on the 60 m channel at " + formatMHz(centre)
               + ": sending allowed, at up to 100 W ERP (47 CFR 97.313(i))";
    const Span* s = insideSegment(dialHz);
    if (s == nullptr) return std::string();
    if (s->lowHz == us60mSegment().lowHz)
        return "Inside the 60 m band segment: sending allowed, at up to 9.15 W ERP, 15 W EIRP (47 CFR 97.313(i))";
    return "Inside a US data segment: sending allowed";
}

} // namespace radio
} // namespace ragtime
