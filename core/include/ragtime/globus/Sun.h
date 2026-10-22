// Sunlight on Globus: where the sun is overhead, how lit each point of the
// globe is, and the time the lighting is for.
//
// The globe is lit from the subsolar point, the place the sun is straight
// overhead. A point whose zenith is the angle z from the sun has the sun at
// altitude 90° - z, so the sine of the sun's altitude there is cos z, the
// dot product of the point's unit vector with the subsolar point's. Call it
// the sun's height. By day (height above sin 3°) the colours are lit
// lightly by Lambert's cosine law, 0.93 + 0.07 cos z. By night (below
// sin -12°, past nautical twilight) they are dimmed to 0.30 of themselves
// and washed with navy. A smoothstep between the two is the greyline, the
// band of dawn and dusk that HF operators watch. Last, the globe darkens a
// little towards its rim, 0.88 + 0.12 × depth, so it looks round.
// docs/MOCKUPS.md has the figures and tools/globe/render_sunlit_globe.py
// the reference renderer.
#ifndef RAGTIME_GLOBUS_SUN_H
#define RAGTIME_GLOBUS_SUN_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "ragtime/globus/Globe.h"

namespace ragtime::globus
{

// Where the sun is overhead at a moment, in seconds since 1970 UTC: the
// sun's declination is the latitude, and the longitude is where its right
// ascension meets the sidereal time. Good to a few hundredths of a degree
// this century, far finer than a pixel of the globe.
LatLon subsolarPoint(std::int64_t unixSeconds);

// How much of the day's light a point has, from its sun height (the sine of
// the sun's altitude there): 0 at night, 1 by day, a smoothstep between
// 12° below the horizon and 3° above.
double daylightShare(double sunHeight);

// What the light does to a colour at a point of the globe, its sun height
// and its depth (view z, 1 in the middle of the globe and 0 at the rim):
// lit, the colour is colour × keep + NIGHT × night, channel by channel.
struct Shade
{
    double keep = 1.0;
    double night = 0.0;
};
Shade shadeAt(double sunHeight, double depth);

// The navy the night side is washed with.
struct Rgb
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};
constexpr Rgb NIGHT{28, 44, 86};

// The light as a picture to lay over the globe once the sea and land are
// filled in plain colours, so a drawing library can paint it with ordinary
// alpha blending: over a colour c, colour × alpha + c × (1 - alpha) is
// c × keep + NIGHT × night, which is what shadeAt asks for. Unmultiplied
// RGB and alpha in separate rows of bytes, as wxImage keeps them; nothing
// off the globe, and its rim smoothed over a pixel. pixelScale is pixels
// per window unit, 2 on a doubled-up screen.
struct Overlay
{
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    std::vector<unsigned char> alpha;
};
void lightOverlay(const View& view, const Window& window, const LatLon& sun, double pixelScale, Overlay& out);

// The seconds a local time is ahead of UTC (negative west of Greenwich),
// from the same moment broken down both ways, as std::localtime and
// std::gmtime give it. Daylight time is in it.
int utcOffsetSeconds(const std::tm& local, const std::tm& utc);

// A short name for a time zone: "PDT" stays "PDT"; a long name, as Windows
// gives, becomes its initials, "Pacific Daylight Time" to "PDT".
std::string zoneAbbreviation(const std::string& zoneName);

// The time the globe is lit for, for its header: "22:36 UTC / 15:36 PDT",
// or just "22:36 UTC" when local time is UTC.
std::string lightingTime(std::int64_t unixSeconds, int utcOffsetSeconds, const std::string& zoneName);

} // namespace ragtime::globus

#endif // RAGTIME_GLOBUS_SUN_H
