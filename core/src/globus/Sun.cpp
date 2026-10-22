// Sunlight on Globus.
#include "ragtime/globus/Sun.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "ragtime/globus/Globe.h"

namespace ragtime::globus
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr double DEGREES = 180.0 / PI;

// 2000-01-01 12:00 UTC, the epoch J2000 the sun's figures count from.
constexpr std::int64_t J2000_UNIX = 946728000;

// The light, as docs/MOCKUPS.md gives it.
constexpr double DAWN_BELOW_DEGREES = -12.0;   // night ends
constexpr double DAY_ABOVE_DEGREES = 3.0;      // full day begins
constexpr double DAY_BASE = 0.93;
constexpr double DAY_LAMBERT = 0.07;
constexpr double NIGHT_KEEP = 0.30;
constexpr double NIGHT_WASH = 0.55;
constexpr double LIMB_BASE = 0.88;
constexpr double LIMB_DEPTH = 0.12;

double wrapDegrees(double degrees)
{
    degrees = std::fmod(degrees, 360.0);
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

unsigned char toByte(double value)
{
    return (unsigned char)std::lround(std::clamp(value, 0.0, 255.0));
}

std::string twoDigits(int value)
{
    char text[8];
    std::snprintf(text, sizeof text, "%02d", value);
    return text;
}

// "hh:mm" of a moment shifted by an offset, on the 24-hour clock.
std::string clock(std::int64_t unixSeconds, int offsetSeconds)
{
    std::int64_t minutes = (unixSeconds + offsetSeconds) / 60;
    if ((unixSeconds + offsetSeconds) % 60 < 0) minutes--;
    int ofDay = (int)(((minutes % 1440) + 1440) % 1440);
    return twoDigits(ofDay / 60) + ":" + twoDigits(ofDay % 60);
}

} // namespace

LatLon subsolarPoint(std::int64_t unixSeconds)
{
    // The low-precision solar coordinates of the Astronomical Almanac:
    // the sun's mean longitude and mean anomaly, the ecliptic longitude
    // from them, then right ascension and declination on the tilted
    // ecliptic. The longitude overhead is the right ascension less the
    // Greenwich mean sidereal time.
    double n = (double)(unixSeconds - J2000_UNIX) / 86400.0;
    double meanLongitude = wrapDegrees(280.460 + 0.9856474 * n) / DEGREES;
    double meanAnomaly = wrapDegrees(357.528 + 0.9856003 * n) / DEGREES;
    double ecliptic = meanLongitude + (1.915 * std::sin(meanAnomaly) + 0.020 * std::sin(2.0 * meanAnomaly)) / DEGREES;
    double tilt = (23.439 - 0.0000004 * n) / DEGREES;

    double declination = std::asin(std::sin(tilt) * std::sin(ecliptic));
    double rightAscension = std::atan2(std::cos(tilt) * std::sin(ecliptic), std::cos(ecliptic));
    double siderealDegrees = wrapDegrees(280.46061837 + 360.98564736629 * n);
    double lon = wrapDegrees(rightAscension * DEGREES - siderealDegrees + 180.0) - 180.0;
    return LatLon{declination * DEGREES, lon};
}

double daylightShare(double sunHeight)
{
    const double from = std::sin(DAWN_BELOW_DEGREES / DEGREES);
    const double to = std::sin(DAY_ABOVE_DEGREES / DEGREES);
    double t = std::clamp((sunHeight - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Shade shadeAt(double sunHeight, double depth)
{
    double day = daylightShare(sunHeight);
    double lit = DAY_BASE + DAY_LAMBERT * std::clamp(sunHeight, 0.0, 1.0);
    double limb = LIMB_BASE + LIMB_DEPTH * std::clamp(depth, 0.0, 1.0);
    Shade shade;
    shade.keep = (lit * day + NIGHT_KEEP * (1.0 - day)) * limb;
    shade.night = NIGHT_WASH * (1.0 - day) * limb;
    return shade;
}

void lightOverlay(const View& view, const Window& window, const LatLon& sun, double pixelScale, Overlay& out)
{
    pixelScale = pixelScale > 0.0 ? pixelScale : 1.0;
    out.width = std::max(0, (int)std::lround(window.width * pixelScale));
    out.height = std::max(0, (int)std::lround(window.height * pixelScale));
    const size_t pixels = (size_t)out.width * (size_t)out.height;
    out.rgb.assign(pixels * 3, 0);
    out.alpha.assign(pixels, 0);
    if (pixels == 0 || view.radius <= 0.0) return;

    // The sun in view axes, so each pixel needs only a dot product.
    const Vec3 s = toView(view.attitude, toVector(sun));
    const double radius = view.radius * pixelScale;
    const double cx = window.width * pixelScale / 2.0;
    const double cy = window.height * pixelScale / 2.0;

    // Only the rows and columns the globe covers.
    const int top = std::max(0, (int)std::floor(cy - radius - 1.0));
    const int bottom = std::min(out.height, (int)std::ceil(cy + radius + 1.0));
    const int left = std::max(0, (int)std::floor(cx - radius - 1.0));
    const int right = std::min(out.width, (int)std::ceil(cx + radius + 1.0));
    for (int py = top; py < bottom; py++)
    {
        double y = (cy - (py + 0.5)) / radius;
        for (int px = left; px < right; px++)
        {
            double x = (px + 0.5 - cx) / radius;
            double off = std::sqrt(x * x + y * y);
            // How much of the pixel the globe covers, near enough.
            double cover = std::clamp((1.0 - off) * radius + 0.5, 0.0, 1.0);
            if (cover <= 0.0) continue;
            double u = off > 1.0 ? x / off : x;
            double v = off > 1.0 ? y / off : y;
            double depth = std::sqrt(std::max(0.0, 1.0 - u * u - v * v));
            Shade shade = shadeAt(u * s.x + v * s.y + depth * s.z, depth);

            double alpha = std::clamp(1.0 - shade.keep, 0.0, 1.0);
            size_t at = (size_t)py * (size_t)out.width + (size_t)px;
            out.alpha[at] = toByte(alpha * cover * 255.0);
            if (alpha > 1e-6)
            {
                double share = shade.night / alpha;
                out.rgb[3 * at] = toByte(NIGHT.r * share);
                out.rgb[3 * at + 1] = toByte(NIGHT.g * share);
                out.rgb[3 * at + 2] = toByte(NIGHT.b * share);
            }
        }
    }
}

int utcOffsetSeconds(const std::tm& local, const std::tm& utc)
{
    // The two are at most a day apart, so across a new year the later one
    // is in the later year.
    int days = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) days = local.tm_year > utc.tm_year ? 1 : -1;
    return days * 86400 + (local.tm_hour - utc.tm_hour) * 3600 + (local.tm_min - utc.tm_min) * 60
           + (local.tm_sec - utc.tm_sec);
}

std::string zoneAbbreviation(const std::string& zoneName)
{
    if (zoneName.find(' ') == std::string::npos) return zoneName;
    std::string initials;
    bool start = true;
    for (char c : zoneName)
    {
        if (c == ' ')
        {
            start = true;
            continue;
        }
        if (start && std::isalpha((unsigned char)c)) initials += (char)std::toupper((unsigned char)c);
        start = false;
    }
    return initials;
}

std::string lightingTime(std::int64_t unixSeconds, int utcOffsetSeconds, const std::string& zoneName)
{
    std::string utc = clock(unixSeconds, 0) + " UTC";
    std::string zone = zoneAbbreviation(zoneName);
    if (utcOffsetSeconds == 0 && (zone.empty() || zone == "UTC" || zone == "GMT" || zone == "CUT")) return utc;
    if (zone.empty()) zone = "local";
    return utc + " / " + clock(unixSeconds, utcOffsetSeconds) + " " + zone;
}

} // namespace ragtime::globus
