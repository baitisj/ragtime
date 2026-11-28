#include "ragtime/radio/RigFilter.h"

#include <cmath>
#include <cstdio>
#include <string>

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

std::string ifHz(int hz)
{
    char text[32];
    std::snprintf(text, sizeof text, "%+d Hz", hz);
    return text;
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
    if (filter.ifShiftHz && !filter.pbtIn && !filter.pbtOut)
        out += (out.empty() ? "" : dot) +
               (*filter.ifShiftHz == 0 ? std::string("IF shift centred") : "IF shift " + ifHz(*filter.ifShiftHz));
    if (filter.notchOn && *filter.notchOn)
    {
        char text[32];
        if (filter.notchRaw)
            std::snprintf(text, sizeof text, "notch %.2f", *filter.notchRaw);
        else if (filter.notchHz)
            std::snprintf(text, sizeof text, "notch %d Hz", *filter.notchHz);
        else
            std::snprintf(text, sizeof text, "notch on");
        out += (out.empty() ? "" : dot) + std::string(text);
        if (filter.notchWidth && filter.notchWidths > 1)
            out += " (width " + std::to_string(*filter.notchWidth + 1) + " of " + std::to_string(filter.notchWidths) + ")";
    }
    return out;
}

std::string ifShiftText(const RigFilter& filter)
{
    return filter.ifShiftHz ? "IF " + ifHz(*filter.ifShiftHz) : std::string();
}

std::string notchText(const RigFilter& filter)
{
    if (!filter.notchOn) return std::string();
    if (!*filter.notchOn) return "NF off";
    const std::string width = filter.notchWidth && filter.notchWidths > 1
                                  ? " W" + std::to_string(*filter.notchWidth) + "/" + std::to_string(filter.notchWidths)
                                  : std::string();
    if (filter.notchRaw) return "NF " + pbtText(*filter.notchRaw) + width;
    if (filter.notchHz) return "NF " + std::to_string(*filter.notchHz) + " Hz" + width;
    return "NF on" + width;
}

bool notchWidthName(const std::string& name)
{
    auto has = [&name](const char* part) { return name.find(part) != std::string::npos; };
    const bool endsW = name.size() >= 2 && name.compare(name.size() - 2, 2, "_W") == 0;
    return has("NOTCH") && (has("WIDTH") || endsW) && !has("APF") && !has("CONTOUR");
}

} // namespace radio
} // namespace ragtime
