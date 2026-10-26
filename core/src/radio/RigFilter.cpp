#include "ragtime/radio/RigFilter.h"

#include <cmath>
#include <cstdio>

namespace ragtime
{
namespace radio
{

namespace
{

bool knob(double value)
{
    return std::fabs(value) <= 1.0;
}

} // namespace

bool pbtCentred(double value)
{
    // A knob's middle is 128 / 255; one step either side is still the middle.
    return knob(value) ? std::fabs(value - 0.5) <= 1.5 / 255.0 : std::fabs(value) < 1.0;
}

std::string pbtText(double value)
{
    char text[32];
    if (knob(value))
        std::snprintf(text, sizeof text, "%.2f", value);
    else
        std::snprintf(text, sizeof text, "%+.0f Hz", value);
    return text;
}

std::string describe(const RigFilter& filter)
{
    const std::string dot = " \xc2\xb7 ";
    std::string out = filter.mode;
    if (filter.widthHz > 0) out += (out.empty() ? "" : dot) + std::to_string(filter.widthHz) + " Hz filter";
    if (filter.pbtIn || filter.pbtOut)
    {
        const bool centred = (!filter.pbtIn || pbtCentred(*filter.pbtIn)) && (!filter.pbtOut || pbtCentred(*filter.pbtOut));
        std::string pbt = "PBT centred";
        if (!centred)
        {
            pbt = "PBT";
            if (filter.pbtIn) pbt += " in " + pbtText(*filter.pbtIn);
            if (filter.pbtIn && filter.pbtOut) pbt += ",";
            if (filter.pbtOut) pbt += " out " + pbtText(*filter.pbtOut);
        }
        out += (out.empty() ? "" : dot) + pbt;
    }
    return out;
}

} // namespace radio
} // namespace ragtime
