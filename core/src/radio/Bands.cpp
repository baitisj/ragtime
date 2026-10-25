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

std::string usDataRefusal(uint64_t dialHz)
{
    const Span* nearest = nullptr;
    uint64_t nearestHz = UINT64_MAX;
    for (const Span& s : usDataSegments())
    {
        if (dialHz >= s.lowHz + SEGMENT_MARGIN_HZ && dialHz + SEGMENT_MARGIN_HZ <= s.highHz) return std::string();
        uint64_t away = dialHz < s.lowHz ? s.lowHz - dialHz : dialHz > s.highHz ? dialHz - s.highHz : 0;
        if (away < nearestHz)
        {
            nearest = &s;
            nearestHz = away;
        }
    }
    std::string why = formatMHz(dialHz);
    std::string band = bandOf(dialHz);
    if (band == "60 m")
        why += " is on 60 m, where data is only on four 2.8 kHz channels (47 CFR 97.303(h)); Ragtime "
               "can't tell from the dial whether a burst sits inside one.";
    else
        why += " is outside the US data segments (47 CFR 97.305).";
    if (nearest != nullptr)
    {
        std::string range = formatMHz(nearest->lowHz);
        range = range.substr(0, range.size() - 4) + "-" + formatMHz(nearest->highHz);
        why += " The nearest is " + range + " (" + nearest->band + "), with 3 kHz to spare at each edge.";
    }
    return why;
}

} // namespace radio
} // namespace ragtime
