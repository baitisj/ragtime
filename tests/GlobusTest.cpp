// Checks the sums behind Globus: locators, distances and captions, the
// framing and rolling ported from Glissando's map ball, the outlines and
// map tables, and the sunlight.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "Admin1Lines.h"
#include "BorderLines.h"
#include "LakeOutlines.h"
#include "Check.h"
#include "ragtime/globus/Globe.h"
#include "ragtime/globus/Sun.h"

using namespace ragtime::globus;

namespace
{

// Glissando's map ball: a window four times as wide as it is tall, and the
// zoom it frames paths with. The framing and rolling are checked there, as
// in Glissando, as well as in Globus's square window.
const Window STRIP{416.0, 104.0};
const Zoom STRIP_ZOOM{1200.0, 50.0, 245.0, 1200.0};

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

LatLon centreOf(const char* locator)
{
    LatLon position;
    CHECK(locatorCentre(locator, position));
    return position;
}

void testLocators()
{
    LatLon square = centreOf("CN87");
    CHECK(near(square.lat, 47.5, 1e-9));
    CHECK(near(square.lon, -123.0, 1e-9));

    LatLon subsquare = centreOf("cn87UX");
    CHECK(near(subsquare.lat, 47.0 + 23.5 / 24.0, 1e-9));
    CHECK(near(subsquare.lon, -124.0 + 20.5 / 12.0, 1e-9));

    LatLon corner = centreOf("AA00");
    CHECK(near(corner.lat, -89.5, 1e-9) && near(corner.lon, -179.0, 1e-9));
    LatLon other = centreOf("RR99");
    CHECK(near(other.lat, 89.5, 1e-9) && near(other.lon, 179.0, 1e-9));

    LatLon unused;
    CHECK(!locatorCentre("", unused));
    CHECK(!locatorCentre("CN8", unused));
    CHECK(!locatorCentre("SN87", unused));   // fields stop at R
    CHECK(!locatorCentre("CN87zz", unused)); // subsquares stop at x
    CHECK(!locatorCentre("CN87u", unused));
}

void testDistanceAndBearing()
{
    // Seattle's square to Berlin's: about 8,100 km, leaving a little east
    // of north over Greenland.
    LatLon seattle = centreOf("CN87");
    LatLon berlin = centreOf("JO62");
    CHECK(near(distanceKm(seattle, berlin), 8139.0, 5.0));
    CHECK(near(bearingDegrees(seattle, berlin), 26.0, 1.0));
    CHECK(near(distanceKm(berlin, seattle), distanceKm(seattle, berlin), 1e-6));

    // A degree of latitude is about 111 km, due north or south.
    LatLon a{10.0, 20.0};
    LatLon b{11.0, 20.0};
    CHECK(near(distanceKm(a, b), 111.19, 0.05));
    CHECK(near(bearingDegrees(a, b), 0.0, 1e-6));
    CHECK(near(bearingDegrees(b, a), 180.0, 1e-6));

    // Due west comes out as 270, not -90.
    CHECK(near(bearingDegrees(LatLon{0.0, 10.0}, LatLon{0.0, 0.0}), 270.0, 1e-6));
}

void testCaption()
{
    LatLon home = centreOf("CN87");
    LatLon there = centreOf("FN31");
    const std::string dot = " \xC2\xB7 ";
    std::string distance = std::to_string(std::lround(distanceKm(home, there)));
    distance.insert(distance.size() - 3, ",");
    std::string expected = "W1AW" + dot + "FN31" + dot + distance + " km" + dot
                           + std::to_string(std::lround(bearingDegrees(home, there))) + "\xC2\xB0";
    CHECK_EQ(caption("CN87", "W1AW", "FN31"), expected);
    CHECK_EQ(caption("cn87", "W1AW", "fn31"), expected);
    // The mock-up's figures.
    CHECK_EQ(caption("CN87", "W1AW", "FN31"), "W1AW" + dot + "FN31" + dot + "3,951 km" + dot + "81\xC2\xB0");

    // Thousands get a separator, and the callsign can be left out.
    std::string far = caption("CN87", "", "JO62");
    CHECK(far.find("8,139 km") != std::string::npos);
    CHECK(far.rfind("JO62", 0) == 0);
    CHECK_EQ(withThousands(12345678), std::string("12,345,678"));
    CHECK_EQ(withThousands(999), std::string("999"));

    CHECK_EQ(caption("", "W1AW", "FN31"), "W1AW" + dot + "FN31");     // no home: just where they are
    CHECK_EQ(caption("CN87", "W1AW", ""), std::string("W1AW"));        // their square not known
    CHECK_EQ(caption("CN87", "", ""), std::string("CN87"));            // nobody yet: where we are
    CHECK_EQ(caption("CN87ux", "", ""), std::string("CN87ux"));
    CHECK_EQ(caption("CN87", "K7ABC", "CN87"), "K7ABC" + dot + "CN87"); // same square
    CHECK(caption("", "", "").empty());
    CHECK_EQ(caption("bogus", "", "FN31"), std::string("FN31"));

    // A square kept from an earlier contact says so.
    CHECK_EQ(caption("CN87", "W1AW", "FN31", false),
             "W1AW" + dot + "FN31 (last contact)" + dot + distance + " km" + dot
                 + std::to_string(std::lround(bearingDegrees(home, there))) + "\xC2\xB0");
    CHECK_EQ(caption("", "", "FN31", false), std::string("FN31 (last contact)"));
    CHECK_EQ(caption("CN87", "", "", false), std::string("CN87"));

    CHECK_EQ(tidyLocator(" cn87UX "), std::string("CN87ux"));
    CHECK(tidyLocator("CN8").empty());
}

void testLand()
{
    CHECK(isLand(LatLon{39.0, -98.0}));     // Kansas
    CHECK(isLand(LatLon{23.0, 10.0}));      // the Sahara
    CHECK(isLand(LatLon{-25.0, 134.0}));    // Australia
    CHECK(isLand(LatLon{-85.0, 0.0}));      // Antarctica
    CHECK(isLand(LatLon{72.0, -40.0}));     // Greenland
    CHECK(!isLand(LatLon{0.0, -150.0}));    // the Pacific
    CHECK(!isLand(LatLon{30.0, -40.0}));    // the Atlantic
    CHECK(!isLand(LatLon{-30.0, 80.0}));    // the Indian Ocean
    CHECK(!isLand(LatLon{0.0, 180.0}));     // the date line wraps
    CHECK(isLand(LatLon{66.0, 178.0}));     // Chukotka, both sides of it
    CHECK(isLand(LatLon{66.0, -178.0}));
    CHECK(isLand(LatLon{-28.0, 25.0}));     // South Africa, and Lesotho in the
    CHECK(isLand(LatLon{-29.5, 28.2}));     // hole in it
}

// Whether a point of the window is inside the outlines, by the non-zero rule.
bool filled(const std::vector<Outline>& outlines, double x, double y)
{
    int winding = 0;
    for (const Outline& outline : outlines)
    {
        for (size_t i = 0; i < outline.size(); i++)
        {
            const Point& a = outline[i];
            const Point& b = outline[(i + 1) % outline.size()];
            double side = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
            if (a.y <= y && b.y > y && side > 0.0) winding++;
            else if (a.y > y && b.y <= y && side < 0.0) winding--;
        }
    }
    return winding != 0;
}

void testOutlines()
{
    // Drawn outlines agree with the land, point for point, however the ball
    // is turned and however close: whole continents, land going round the
    // back of the ball at its rim, the poles, the date line.
    struct Case
    {
        double lat, lon, roll, radius;
    } cases[] = {
        {0.0, -150.0, 0.0, 180.0}, {39.0, -98.0, 0.0, 245.0}, {-89.0, 0.0, 0.0, 180.0},
        {65.0, 180.0, 0.4, 180.0}, {0.0, 20.0, 0.0, 180.0},   {52.0, 13.0, 0.0, 2000.0},
        {20.0, 80.0, 1.0, 120.0},  {10.0, -30.0, 2.5, 60.0},
    };
    Window window{400.0, 400.0};
    for (const Case& c : cases)
    {
        View view{lookingAt(toVector(LatLon{c.lat, c.lon}), c.roll), c.radius};
        std::vector<Outline> outlines = landOutlines(view, window);
        int shown = 0;
        int wrong = 0;
        for (int py = 0; py < 400; py += 4)
        {
            for (int px = 0; px < 400; px += 4)
            {
                double x = px + 0.5;
                double y = py + 0.5;
                double sx = (x - 200.0) / c.radius;
                double sy = (200.0 - y) / c.radius;
                double r2 = sx * sx + sy * sy;
                if (r2 > 0.99) continue;
                Vec3 face{sx, sy, std::sqrt(1.0 - r2)};
                shown++;
                if (filled(outlines, x, y) != isLand(toLatLon(toEarth(view.attitude, face)))) wrong++;
            }
        }
        CHECK(shown > 0);
        CHECK(wrong <= shown / 500);
    }

    // Seen from far over the Pacific, nothing at all is drawn for the land
    // round the back.
    View pacific{lookingAt(toVector(LatLon{0.0, -150.0})), 50.0};
    Window small{120.0, 120.0};
    CHECK(!filled(landOutlines(pacific, small), 60.0, 60.0));

    // The graticule stays on the near side of the ball: from over Kansas,
    // with the north pole in sight, every meridian shows, and the parallels
    // from 60 north to 30 south.
    View kansas{lookingAt(toVector(LatLon{39.0, -98.0})), 50.0};
    std::vector<Outline> lines = graticule(kansas, small);
    CHECK(lines.size() >= 16 && lines.size() <= 24);
    for (const Outline& line : lines)
    {
        for (const Point& p : line) CHECK(std::hypot(p.x - 60.0, p.y - 60.0) <= 50.0 + 1e-6);
    }
    CHECK(graticule(kansas, small, 10).size() > 2 * lines.size());

    // Where a state line runs along the graticule, the graticule makes way
    // for it: down 120 W between California and Nevada, from 39 N to 42 N,
    // there is none, but south of there, where the line turns off down to
    // the Colorado, the meridian shows again.
    auto nearest = [](const std::vector<Outline>& outlines, const Point& p) {
        double best = 1e9;
        for (const Outline& line : outlines)
        {
            for (size_t i = 0; i + 1 < line.size(); i++)
            {
                double dx = line[i + 1].x - line[i].x, dy = line[i + 1].y - line[i].y;
                double length = dx * dx + dy * dy;
                double t = length > 0.0 ? std::clamp(((p.x - line[i].x) * dx + (p.y - line[i].y) * dy) / length, 0.0, 1.0) : 0.0;
                best = std::min(best, std::hypot(line[i].x + dx * t - p.x, line[i].y + dy * t - p.y));
            }
        }
        return best;
    };
    Window close{400.0, 400.0};
    View sierra{lookingAt(toVector(LatLon{39.0, -120.0})), 4000.0};
    std::vector<Outline> grid = graticule(sierra, close);
    CHECK(nearest(grid, onScreen(sierra, close, toVector(LatLon{40.5, -120.0}))) > 20.0);
    CHECK(nearest(grid, onScreen(sierra, close, toVector(LatLon{37.5, -120.0}))) < 1.0);
    CHECK(nearest(admin1Lines(sierra, close), onScreen(sierra, close, toVector(LatLon{40.5, -120.0}))) < 1.0);

    // The coasts are the land's edges: every point of them is on the near
    // side, and over North America there are plenty.
    std::vector<Outline> coasts = coastLines(kansas, small);
    CHECK(coasts.size() > 10);
    for (const Outline& line : coasts)
    {
        for (const Point& p : line) CHECK(std::hypot(p.x - 60.0, p.y - 60.0) <= 50.0 + 1e-6);
    }
    View midPacific{lookingAt(toVector(LatLon{0.0, -140.0})), 2000.0};
    CHECK(coastLines(midPacific, small).empty());
}

void testAttitude()
{
    LatLon home = centreOf("CN87");
    Attitude a = lookingAt(toVector(home));

    // Home is in the middle, north is up and east is to the right.
    Vec3 middle = toView(a, toVector(home));
    CHECK(near(middle.x, 0.0, 1e-9) && near(middle.y, 0.0, 1e-9) && near(middle.z, 1.0, 1e-9));
    Vec3 north = toView(a, toVector(LatLon{home.lat + 5.0, home.lon}));
    CHECK(north.y > 0.05 && near(north.x, 0.0, 1e-9));
    Vec3 east = toView(a, toVector(LatLon{home.lat, home.lon + 5.0}));
    CHECK(east.x > 0.05);

    // Turned clockwise a quarter: north is to the right.
    Attitude turned = lookingAt(toVector(home), std::acos(-1.0) / 2.0);
    Vec3 northTurned = toView(turned, toVector(LatLon{home.lat + 5.0, home.lon}));
    CHECK(northTurned.x > 0.05 && near(northTurned.y, 0.0, 1e-9));
    CHECK(near(angleBetween(a, turned), std::acos(-1.0) / 2.0, 1e-9));
    CHECK(near(angleBetween(a, a), 0.0, 1e-6));

    // And back again.
    Vec3 v = toVector(LatLon{12.0, 34.0});
    Vec3 back = toEarth(turned, toView(turned, v));
    CHECK(near(back.x, v.x, 1e-12) && near(back.y, v.y, 1e-12) && near(back.z, v.z, 1e-12));

    // Straight down on a pole still gives a usable attitude.
    Attitude pole = lookingAt(Vec3{0.0, 0.0, 1.0});
    CHECK(near(pole.out.z, 1.0, 1e-12) && near(pole.up.z, 0.0, 1e-12));
}

void testGreatCircle()
{
    Vec3 a = toVector(LatLon{0.0, 0.0});
    Vec3 b = toVector(LatLon{0.0, 90.0});
    std::vector<Vec3> path = greatCircle(a, b, 4);
    CHECK(path.size() == 5);
    LatLon middle = toLatLon(path[2]);
    CHECK(near(middle.lat, 0.0, 1e-9) && near(middle.lon, 45.0, 1e-9));

    // Opposite points still make a path half way round.
    std::vector<Vec3> round = greatCircle(a, toVector(LatLon{0.0, 180.0}), 8);
    LatLon end = toLatLon(round.back());
    CHECK(near(std::fabs(end.lon), 180.0, 1e-6));
    for (const Vec3& p : round) CHECK(near(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), 1.0, 1e-9));
}

void testFraming()
{
    Window window = STRIP;
    Zoom zoom = STRIP_ZOOM;

    // A short path is shown whole, north up, as close as it can be, curling
    // over the ball's middle: not drawn straight through it.
    LatLon home = centreOf("CN87");
    LatLon near_ = centreOf("DN06");
    std::vector<Vec3> path = greatCircle(toVector(home), toVector(near_), 64);
    View v = framePath(home, near_, window, zoom);
    CHECK(pointsInView(v, path, window) == (int)path.size());
    CHECK(near(v.attitude.right.z, 0.0, 1e-9));      // right is level: north is up
    CHECK(v.radius > 4.0 * zoom.standard);

    // So is Europe from Seattle, over the pole, farther out, and the path
    // bows towards the pole rather than running through the centre.
    LatLon berlin = centreOf("JO62");
    path = greatCircle(toVector(home), toVector(berlin), 64);
    View europe = framePath(home, berlin, window, zoom);
    CHECK(pointsInView(europe, path, window) == (int)path.size());
    CHECK(europe.radius < v.radius);
    Vec3 middle = toView(europe.attitude, path[32]);
    CHECK(middle.y * europe.radius > 4.0);             // above the centre, towards the pole

    // North to south, 8,600 km, does not fit the wide window north up: the
    // ball turns to show all of it closer than north up could.
    LatLon connecticut = centreOf("FN31");
    LatLon uruguay = centreOf("GF15");
    path = greatCircle(toVector(connecticut), toVector(uruguay), 64);
    View south = framePath(connecticut, uruguay, window, zoom);
    CHECK(std::fabs(south.attitude.right.z) > 0.2);
    CHECK(pointsInView(south, path, window) == (int)path.size());
    View upright{lookingAt(south.attitude.out), south.radius};
    CHECK(pointsInView(upright, path, window) < (int)path.size());

    // Never closer than nearest nor farther than farthest.
    for (const View& each : {v, europe, south}) CHECK(each.radius <= zoom.nearest && each.radius >= zoom.farthest);

    // The same square at both ends: just look at it.
    View same = framePath(home, home, window, zoom);
    Vec3 there = toView(same.attitude, toVector(home));
    CHECK(near(there.z, 1.0, 1e-9));
    CHECK(near(same.radius, zoom.standard, 1e-9));

    // Opposite sides of the earth: something sensible, not a crash.
    View opposite = framePath(LatLon{10.0, 20.0}, LatLon{-10.0, -160.0}, window, zoom);
    CHECK(std::isfinite(opposite.attitude.out.x) && std::isfinite(opposite.attitude.right.y));
    CHECK(std::isfinite(opposite.radius));
}

void testRolling()
{
    View from{lookingAt(toVector(centreOf("CN87"))), 600.0};
    View to{lookingAt(toVector(centreOf("JO62")), 0.3), 330.0};

    Roller roller;
    roller.setBackOff(208.0);
    roller.jump(from);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.view().attitude, from.attitude), 0.0, 1e-6));
    CHECK(near(roller.view().radius, 600.0, 1e-9));

    // It eases in, never overshoots, backs off on the way, and is still in
    // a few seconds, where it was sent.
    roller.rollTo(to);
    double last = angleBetween(roller.view().attitude, to.attitude);
    double atHalfSecond = 0.0;
    double farthest = roller.view().radius;
    bool steady = true;
    double t = 0.0;
    while (roller.moving() && t < 6.0)
    {
        roller.step(1.0 / 60.0);
        t += 1.0 / 60.0;
        double left = angleBetween(roller.view().attitude, to.attitude);
        if (left > last + 1e-9) steady = false;
        last = left;
        farthest = std::min(farthest, roller.view().radius);
        if (near(t, 0.5, 0.009)) atHalfSecond = left;
    }
    CHECK(steady);
    CHECK(!roller.moving());
    CHECK(t > 1.2 && t < 4.0);
    CHECK(atHalfSecond > 0.2 * angleBetween(from.attitude, to.attitude));     // not a jump
    CHECK(farthest < 0.5 * from.radius && farthest < to.radius);               // backed off
    CHECK(near(angleBetween(roller.view().attitude, to.attitude), 0.0, 1e-6));
    CHECK(near(roller.view().radius, 330.0, 1e-6));

    // Zooming in place is just as smooth, and does not back off.
    View closer = to;
    closer.radius = 600.0;
    roller.rollTo(closer);
    double smallest = 1e9;
    for (int i = 0; i < 600 && roller.moving(); i++)
    {
        roller.step(1.0 / 60.0);
        smallest = std::min(smallest, roller.view().radius);
    }
    CHECK(!roller.moving());
    CHECK(smallest >= 330.0 - 1e-6);
    CHECK(near(roller.view().radius, 600.0, 1e-6));

    // Sent somewhere new mid roll, it carries on smoothly to there.
    roller.rollTo(from);
    roller.step(0.4);
    roller.rollTo(to);
    for (int i = 0; i < 600 && roller.moving(); i++) roller.step(1.0 / 60.0);
    CHECK(!roller.moving());
    CHECK(near(angleBetween(roller.view().attitude, to.attitude), 0.0, 1e-6));

    // The attitude stays a rotation all the way.
    Attitude now = roller.view().attitude;
    double lengthRight = std::sqrt(now.right.x * now.right.x + now.right.y * now.right.y + now.right.z * now.right.z);
    CHECK(near(lengthRight, 1.0, 1e-9));
}

// How fast the ball turns over the next sixtieth of a second, in radians a
// second.
double spinOver(Roller& roller)
{
    Attitude before = roller.view().attitude;
    roller.step(1.0 / 60.0);
    return angleBetween(before, roller.view().attitude) * 60.0;
}

// Drags the ball from one point to another over a fifth of a second and
// lets go.
void flick(Roller& roller, double fromX, double fromY, double toX, double toY)
{
    CHECK(roller.grab(fromX, fromY));
    for (int i = 1; i <= 12; i++)
    {
        roller.dragTo(fromX + (toX - fromX) * i / 12.0, fromY + (toY - fromY) * i / 12.0);
        roller.step(1.0 / 60.0);
    }
    roller.letGo();
}

void testSpinning()
{
    LatLon cn87 = centreOf("CN87");
    LatLon jo62 = centreOf("JO62");

    // The magnet runs from the southern end of the path to the northern,
    // whichever end is home.
    Vec3 magnet = magnetFor(cn87, jo62);
    Vec3 chord{toVector(jo62).x - toVector(cn87).x, toVector(jo62).y - toVector(cn87).y,
               toVector(jo62).z - toVector(cn87).z};
    double chordLength = std::sqrt(chord.x * chord.x + chord.y * chord.y + chord.z * chord.z);
    CHECK(near(magnet.x * magnet.x + magnet.y * magnet.y + magnet.z * magnet.z, 1.0, 1e-9));
    CHECK(near((magnet.x * chord.x + magnet.y * chord.y + magnet.z * chord.z) / chordLength, 1.0, 1e-9));
    Vec3 swapped = magnetFor(jo62, cn87);
    CHECK(near(swapped.x, magnet.x, 1e-12) && near(swapped.y, magnet.y, 1e-12) && near(swapped.z, magnet.z, 1e-12));

    View home{lookingAt(toVector(cn87)), 245.0};

    // Free, a flick sets it spinning, and it keeps going for a while,
    // slowing, then stops: somewhere else, as nothing draws it back.
    Roller roller;
    roller.setBackOff(208.0);
    roller.jump(home);
    CHECK(roller.fieldOn());
    roller.coast();
    CHECK(!roller.fieldOn());
    flick(roller, -100.0, 0.0, 100.0, 0.0);
    double first = spinOver(roller);
    CHECK(first > 2.0);
    double travelled = 0.0;
    double t = 0.0;
    double atTwoSeconds = 0.0;
    while (roller.moving() && t < 30.0)
    {
        Attitude before = roller.view().attitude;
        roller.step(1.0 / 60.0);
        t += 1.0 / 60.0;
        travelled += angleBetween(before, roller.view().attitude);
        if (near(t, 2.0, 0.009)) atTwoSeconds = angleBetween(before, roller.view().attitude) * 60.0;
    }
    CHECK(atTwoSeconds > 0.3 * first && atTwoSeconds < first);
    CHECK(t > 5.0 && t < 20.0);
    CHECK(travelled > 3.0);
    CHECK(!roller.moving());
    CHECK(near(roller.view().radius, 245.0, 1e-9));

    // A zoom while it coasts leaves it coasting.
    flick(roller, 0.0, -40.0, 0.0, 40.0);
    roller.zoomTo(400.0);
    CHECK(!roller.fieldOn());
    for (int i = 0; i < 60; i++) roller.step(1.0 / 60.0);
    CHECK(spinOver(roller) > 0.5);

    // Held, a spinning ball stops almost at once, with the point pressed
    // on still under the pointer.
    flick(roller, -100.0, 0.0, 100.0, 0.0);
    CHECK(spinOver(roller) > 2.0);
    CHECK(roller.grab(30.0, 10.0));
    t = 0.0;
    while (spinOver(roller) > 0.05 && t < 2.0) t += 1.0 / 60.0;
    CHECK(t < 0.4);
    for (int i = 0; i < 60; i++) roller.step(1.0 / 60.0);
    CHECK(roller.moving());     // held still counts

    // Dragged, the point pressed on follows the pointer.
    roller.letGo();
    View still = roller.view();
    roller.jump(still);
    roller.coast();
    Window window = STRIP;
    double r = still.radius;
    Vec3 under{20.0 / r, 10.0 / r, std::sqrt(1.0 - (400.0 + 100.0) / (r * r))};
    Vec3 grabbed = toEarth(still.attitude, under);
    CHECK(roller.grab(20.0, 10.0));
    roller.dragTo(-60.0, -25.0);
    for (int i = 0; i < 60; i++) roller.step(1.0 / 60.0);
    Point seen = onScreen(roller.view(), window, grabbed);
    CHECK(near(seen.x, window.width / 2.0 - 60.0, 1.0));
    CHECK(near(seen.y, window.height / 2.0 + 25.0, 1.0));
    roller.letGo();

    // Off the ball there is nothing to hold.
    roller.jump(View{home.attitude, 50.0});
    CHECK(!roller.grab(150.0, 0.0));
    CHECK(!roller.held());
    CHECK(roller.grab(20.0, 0.0));
    roller.letGo();

    // Spinning, then sent to a path: the field brakes it far harder than
    // the fluid alone, and it tumbles into the path's view and settles
    // there.
    View path = framePath(cn87, jo62, window, STRIP_ZOOM);
    Roller coasting;
    Roller drawn;
    for (Roller* each : {&coasting, &drawn})
    {
        each->setBackOff(208.0);
        each->jump(home);
        each->coast();
        flick(*each, -100.0, 0.0, 100.0, 0.0);
    }
    drawn.rollTo(path, magnet);
    double coasted = 0.0;
    double tumbled = 0.0;
    t = 0.0;
    while ((coasting.moving() || drawn.moving()) && t < 30.0)
    {
        for (Roller* each : {&coasting, &drawn})
        {
            Attitude before = each->view().attitude;
            each->step(1.0 / 60.0);
            (each == &drawn ? tumbled : coasted) += angleBetween(before, each->view().attitude);
        }
        t += 1.0 / 60.0;
        if (!drawn.moving() && tumbled > 0.0)
        {
            CHECK(t < 5.0);
            tumbled = -tumbled;     // settled: stop counting
        }
    }
    CHECK(!drawn.moving());
    CHECK(-tumbled < coasted / 3.0);
    CHECK(near(angleBetween(drawn.view().attitude, path.attitude), 0.0, 1e-6));
    CHECK(near(drawn.view().radius, path.radius, 1e-6));

    // However it was spinning, and wherever it is sent, it gets there.
    unsigned seed = 7;
    auto random = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0;
    };
    double slowest = 0.0;
    for (int k = 0; k < 200; k++)
    {
        LatLon a{random() * 80.0, random() * 180.0};
        LatLon b{random() * 80.0, random() * 180.0};
        View to = framePath(a, b, window, STRIP_ZOOM);
        Roller each;
        each.setBackOff(208.0);
        each.jump(View{lookingAt(Vec3{random(), random(), random()}, random() * 3.0), 245.0});
        each.coast();
        flick(each, 0.0, 0.0, random() * 200.0, random() * 50.0);
        each.rollTo(to, magnetFor(a, b));
        t = 0.0;
        while (each.moving() && t < 10.0)
        {
            each.step(1.0 / 60.0);
            t += 1.0 / 60.0;
        }
        slowest = std::max(slowest, t);
        CHECK(near(angleBetween(each.view().attitude, to.attitude), 0.0, 1e-6));
    }
    CHECK(slowest < 6.0);
}

const double HALF_TURN = std::acos(-1.0);

// Whether segments pq and rs, in whole hundredths of a degree, meet
// anywhere but at an end they share.
using Corner = std::pair<long long, long long>;

int orientation(const Corner& a, const Corner& b, const Corner& c)
{
    long long v = (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
    return (v > 0) - (v < 0);
}

bool within(const Corner& a, const Corner& b, const Corner& p)
{
    return std::min(a.first, b.first) <= p.first && p.first <= std::max(a.first, b.first) &&
           std::min(a.second, b.second) <= p.second && p.second <= std::max(a.second, b.second);
}

bool meet(const Corner& p, const Corner& q, const Corner& r, const Corner& s)
{
    int sharedEnds = (p == r) + (p == s) + (q == r) + (q == s);
    if (sharedEnds >= 2) return true;
    if (sharedEnds == 1)
    {
        const Corner& x = (p == r || p == s) ? p : q;
        const Corner& a = x == p ? q : p;
        const Corner& b = x == r ? s : r;
        return orientation(x, a, b) == 0 &&
               (a.first - x.first) * (b.first - x.first) + (a.second - x.second) * (b.second - x.second) > 0;
    }
    int o1 = orientation(p, q, r), o2 = orientation(p, q, s), o3 = orientation(r, s, p), o4 = orientation(r, s, q);
    if (o1 != o2 && o3 != o4) return true;
    return (o1 == 0 && within(p, q, r)) || (o2 == 0 && within(p, q, s)) || (o3 == 0 && within(r, s, p)) ||
           (o4 == 0 && within(r, s, q));
}

void testBorders()
{
    struct Segment
    {
        Corner p, q;
    };
    std::vector<Segment> segments;
    for (int l = 0; l < BorderLines::LINE_COUNT; l++)
    {
        for (uint32_t i = BorderLines::LINE_START[l]; i + 1 < BorderLines::LINE_START[l + 1]; i++)
        {
            segments.push_back(Segment{Corner{BorderLines::POINTS[2 * i], BorderLines::POINTS[2 * i + 1]},
                                       Corner{BorderLines::POINTS[2 * i + 2], BorderLines::POINTS[2 * i + 3]}});
        }
    }
    CHECK(segments.size() > 1000);

    // Smoothed, no border crosses another or itself, so they make no
    // countries that aren't there. The one crossing is Natural Earth's own,
    // where Sudan, South Sudan and the Central African Republic meet; it is
    // a few kilometres across.
    auto nearSudanCorner = [](const Segment& a) {
        return std::abs(a.p.first - 2420) < 60 && std::abs(a.p.second - 860) < 60;
    };
    int crossings = 0;
    for (size_t a = 0; a < segments.size(); a++)
    {
        for (size_t b = a + 1; b < segments.size(); b++)
        {
            if (!meet(segments[a].p, segments[a].q, segments[b].p, segments[b].q)) continue;
            if (nearSudanCorner(segments[a]) && nearSudanCorner(segments[b])) continue;
            crossings++;
            std::fprintf(stderr, "borders meet: (%lld, %lld)-(%lld, %lld) and (%lld, %lld)-(%lld, %lld)\n",
                    segments[a].p.first, segments[a].p.second, segments[a].q.first, segments[a].q.second,
                    segments[b].p.first, segments[b].p.second, segments[b].q.first, segments[b].q.second);
        }
    }
    CHECK(crossings == 0);

    // And they run over land, not across bays: every stretch is on land, or
    // right beside it where it follows the edge of the map's data.
    int overSea = 0;
    for (const Segment& segment : segments)
    {
        double dx = (double)(segment.q.first - segment.p.first);
        double dy = (double)(segment.q.second - segment.p.second);
        double length = std::hypot(dx, dy);
        if (length == 0.0) continue;
        for (double t : {0.25, 0.5, 0.75})
        {
            double lon = (segment.p.first + dx * t) / 100.0;
            double lat = (segment.p.second + dy * t) / 100.0;
            double sideLon = -dy / length * 0.01;
            double sideLat = dx / length * 0.01;
            if (!isLand(LatLon{lat, lon}) && !isLand(LatLon{lat + sideLat, lon + sideLon}) &&
                !isLand(LatLon{lat - sideLat, lon - sideLon}))
            {
                overSea++;
            }
        }
    }
    CHECK(overSea == 0);

    // Over Europe there are plenty, all on the near side; over the open
    // Pacific, none.
    Window window{416.0, 104.0};
    View europe{lookingAt(toVector(LatLon{50.0, 10.0})), 600.0};
    std::vector<Outline> lines = borderLines(europe, window);
    CHECK(lines.size() > 20);
    for (const Outline& line : lines)
    {
        for (const Point& p : line) CHECK(std::hypot(p.x - 208.0, p.y - 52.0) <= 600.0 + 1e-6);
    }
    View pacific{lookingAt(toVector(LatLon{-20.0, -150.0})), 500.0};
    CHECK(borderLines(pacific, window).empty());
}

void testGlobusFraming()
{
    // Globus frames a path on the whole globe, so the greyline always
    // shows: the path centred and curling north of the middle, north up.
    Window window{320.0, 320.0};
    Zoom zoom = wholeGlobe(window);
    CHECK(near(zoom.standard, 148.0, 1e-9));
    CHECK(near(zoom.nearest, zoom.farthest, 1e-9));
    CHECK(near(zoom.closest, 8.0 * zoom.standard, 1e-9));

    LatLon home = centreOf("CN87");
    LatLon w1aw = centreOf("FN31");
    std::vector<Vec3> path = greatCircle(toVector(home), toVector(w1aw), 64);
    View view = framePath(home, w1aw, window, zoom);
    CHECK(near(view.radius, zoom.standard, 1e-9));
    CHECK_EQ(pointsInView(view, path, window), (int)path.size());
    CHECK(near(view.attitude.right.z, 0.0, 1e-9));
    Vec3 middle = toView(view.attitude, path[32]);
    CHECK(middle.y > 0.0 && middle.z > 0.9);

    // Even half way round the world, it all shows.
    LatLon far{-35.0, 150.0};
    path = greatCircle(toVector(home), toVector(far), 64);
    View round = framePath(home, far, window, zoom);
    CHECK_EQ(pointsInView(round, path, window), (int)path.size());

    // A window too small for a margin still frames something.
    CHECK(wholeGlobe(Window{10.0, 10.0}).standard > 0.0);
}

void testScaling()
{
    // Resizing the window scales the globe at once, where it is and where
    // it is going, and leaves it as still as it was.
    Roller roller;
    roller.jump(View{lookingAt(toVector(centreOf("CN87"))), 200.0});
    roller.scaleBy(1.5);
    CHECK(near(roller.view().radius, 300.0, 1e-9));
    CHECK(near(roller.target().radius, 300.0, 1e-9));
    CHECK(!roller.moving());
    roller.scaleBy(0.0);
    roller.scaleBy(-2.0);
    CHECK(near(roller.view().radius, 300.0, 1e-9));

    // Mid zoom, it carries on to the scaled radius.
    roller.zoomTo(400.0);
    roller.step(0.1);
    roller.scaleBy(0.5);
    for (int i = 0; i < 600 && roller.moving(); i++) roller.step(1.0 / 60.0);
    CHECK(!roller.moving());
    CHECK(near(roller.view().radius, 200.0, 1e-6));
}

void testLakes()
{
    CHECK_EQ(LakeOutlines::RING_COUNT, 24);

    // The big lakes are water; the 1:110m land has them as land, which is
    // why they are drawn over it.
    for (LatLon wet : {LatLon{43.5, -87.0}, LatLon{47.7, -87.5}, LatLon{44.8, -82.3}, LatLon{42.2, -81.2},
                       LatLon{43.6, -77.8}, LatLon{-1.0, 33.0}, LatLon{61.5, -114.0}})
    {
        CHECK(isLake(wet));
        CHECK(isLand(wet));
    }
    for (LatLon dry : {LatLon{44.5, -89.5}, LatLon{43.0, -84.7}, LatLon{39.0, -98.0}, LatLon{30.0, -40.0}})
    {
        CHECK(!isLake(dry));
    }

    // Lake Michigan parts Wisconsin from Michigan's Lower Peninsula.
    bool crossed = false;
    for (double lon = -89.5; lon <= -84.7; lon += 0.1) crossed = crossed || isLake(LatLon{43.5, lon});
    CHECK(crossed);

    // The drawn lakes agree with isLake, point for point.
    Window window{400.0, 400.0};
    View greatLakes{lookingAt(toVector(LatLon{45.0, -84.0})), 1500.0};
    std::vector<Outline> outlines = lakeOutlines(greatLakes, window);
    CHECK(outlines.size() >= 5);
    int shown = 0;
    int wet = 0;
    int wrong = 0;
    for (int py = 0; py < 400; py += 4)
    {
        for (int px = 0; px < 400; px += 4)
        {
            double x = px + 0.5;
            double y = py + 0.5;
            Vec3 face{(x - 200.0) / 1500.0, (200.0 - y) / 1500.0, 0.0};
            face.z = std::sqrt(1.0 - face.x * face.x - face.y * face.y);
            bool lake = isLake(toLatLon(toEarth(greatLakes.attitude, face)));
            shown++;
            wet += lake;
            if (filled(outlines, x, y) != lake) wrong++;
        }
    }
    CHECK(wet > shown / 20);
    CHECK(wrong <= shown / 500);

    // Close over the open Pacific, no lake is drawn.
    View pacific{lookingAt(toVector(LatLon{0.0, -150.0})), 2000.0};
    CHECK(lakeOutlines(pacific, Window{320.0, 320.0}).empty());
}

void testBordersStopAtLakes()
{
    // The 1:110m borders run on across the big lakes, which the countries
    // fill in as land: through Victoria, down Tanganyika and Malawi, along
    // the middle of the Great Lakes.
    int acrossLakes = 0;
    for (int l = 0; l < BorderLines::LINE_COUNT; l++)
    {
        for (uint32_t i = BorderLines::LINE_START[l]; i + 1 < BorderLines::LINE_START[l + 1]; i++)
        {
            double lon = (BorderLines::POINTS[2 * i] + BorderLines::POINTS[2 * i + 2]) / 200.0;
            double lat = (BorderLines::POINTS[2 * i + 1] + BorderLines::POINTS[2 * i + 3]) / 200.0;
            acrossLakes += isLake(LatLon{lat, lon});
        }
    }
    CHECK(acrossLakes > 10);

    // Drawn, they stop at the shore: the lake, in the sea's colour, is the
    // divide. The borders round the lakes still show.
    Window window{400.0, 400.0};
    for (LatLon centre : {LatLon{-6.0, 31.0}, LatLon{45.0, -82.0}})
    {
        View view{lookingAt(toVector(centre)), 1500.0};
        int edges = 0;
        int wet = 0;
        for (const Outline& line : borderLines(view, window))
        {
            for (size_t i = 0; i + 1 < line.size(); i++)
            {
                Vec3 face{((line[i].x + line[i + 1].x) / 2.0 - 200.0) / 1500.0,
                          (200.0 - (line[i].y + line[i + 1].y) / 2.0) / 1500.0, 0.0};
                face.z = std::sqrt(std::max(0.0, 1.0 - face.x * face.x - face.y * face.y));
                edges++;
                wet += isLake(toLatLon(toEarth(view.attitude, face)));
            }
        }
        CHECK(edges > 10);
        CHECK_EQ(wet, 0);
    }
}

void testCoastsAreNotBorders()
{
    // Over the prairies on the 49th parallel, and over Austria, the window
    // shows no coast, only borders: an edge two countries share is a
    // border, drawn once, smoothed, not stroked as coast as well.
    Window window{400.0, 400.0};
    auto inWindow = [&window](const std::vector<Outline>& lines) {
        int count = 0;
        for (const Outline& line : lines)
        {
            for (const Point& p : line) count += p.x >= 0.0 && p.x <= window.width && p.y >= 0.0 && p.y <= window.height;
        }
        return count;
    };
    for (LatLon inland : {LatLon{49.0, -105.0}, LatLon{48.0, 15.0}})
    {
        View view{lookingAt(toVector(inland)), 6000.0};
        CHECK_EQ(inWindow(coastLines(view, window)), 0);
        CHECK(inWindow(borderLines(view, window)) > 0);
    }

    // Islands and whole coasts still show: Iceland, all coast. The shores
    // of the Great Lakes are lake shores, drawn thin, not coast.
    View iceland{lookingAt(toVector(LatLon{65.0, -18.5})), 3000.0};
    CHECK(!coastLines(iceland, window).empty());
    CHECK(lakeShores(iceland, window).empty());
    View erie{lookingAt(toVector(LatLon{42.2, -81.2})), 6000.0};
    CHECK_EQ(inWindow(coastLines(erie, window)), 0);
    CHECK(inWindow(lakeShores(erie, window)) > 0);

    // Over the whole earth, the coasts are far fewer points than the
    // outlines: about half the outlines' edges are borders.
    size_t coastPoints = 0;
    size_t outlinePoints = 0;
    for (LatLon side : {LatLon{0.0, 0.0}, LatLon{0.0, 90.0}, LatLon{0.0, 180.0}, LatLon{0.0, -90.0}})
    {
        View view{lookingAt(toVector(side)), 100.0};
        for (const Outline& line : coastLines(view, Window{240.0, 240.0})) coastPoints += line.size();
        for (const Outline& line : landOutlines(view, Window{240.0, 240.0})) outlinePoints += line.size();
    }
    CHECK(coastPoints > 0 && coastPoints < outlinePoints * 3 / 4);
}

void testAdmin1()
{
    // The state and province lines, Natural Earth's 1:10m set smoothed,
    // for the United States, Canada, Mexico, Russia, China, India, Brazil,
    // Argentina and Australia: each point in one of those, and on the land
    // or right beside it (they meet the coast at their ends, and run down
    // the middle of the Great Lakes, which the 1:110m land fills in).
    CHECK_EQ(Admin1Lines::LINE_COUNT, 665);
    struct Box
    {
        double south, north, west, east;
    };
    const Box countries[] = {
        {14.0, 84.0, -170.0, -50.0},    // the United States, Canada and Mexico
        {5.0, 82.0, 19.0, 180.0},       // Russia, China and India
        {60.0, 75.0, -180.0, -168.0},   // Chukotka, over the date line
        {-56.0, 6.0, -75.0, -33.0},     // Brazil and Argentina
        {-45.0, -9.0, 112.0, 155.0},    // Australia
    };
    auto inBox = [](const Box& box, double lat, double lon) {
        return lat > box.south && lat < box.north && lon > box.west && lon < box.east;
    };
    int offLand = 0;
    for (int i = 0; i < Admin1Lines::POINT_COUNT; i++)
    {
        double lon = Admin1Lines::POINTS[2 * i] / Admin1Lines::UNITS_PER_DEGREE;
        double lat = Admin1Lines::POINTS[2 * i + 1] / Admin1Lines::UNITS_PER_DEGREE;
        bool inside = false;
        for (const Box& box : countries) inside = inside || inBox(box, lat, lon);
        CHECK(inside);
        bool land = false;
        for (double dlat : {0.0, -0.5, 0.5})
        {
            for (double dlon : {0.0, -0.5, 0.5}) land = land || isLand(LatLon{lat + dlat, lon + dlon});
        }
        if (!land) offLand++;
    }
    CHECK_EQ(offLand, 0);
    CHECK(offLand <= 1);

    // None runs over a lake, and none runs along a country border, which
    // is drawn heavier already: the Saint John River, where Maine meets
    // New Brunswick, is in Natural Earth's admin-1 lines too.
    std::vector<std::pair<Point, Point>> borders;
    for (int l = 0; l < BorderLines::LINE_COUNT; l++)
    {
        for (uint32_t i = BorderLines::LINE_START[l]; i + 1 < BorderLines::LINE_START[l + 1]; i++)
        {
            Point a{BorderLines::POINTS[2 * i] / 100.0, BorderLines::POINTS[2 * i + 1] / 100.0};
            Point b{BorderLines::POINTS[2 * i + 2] / 100.0, BorderLines::POINTS[2 * i + 3] / 100.0};
            borders.push_back({a, b});
        }
    }
    auto toBorder = [&borders](const Point& p) {
        double nearest = 1e9;
        double c = std::cos(p.y * std::acos(-1.0) / 180.0);
        for (const auto& segment : borders)
        {
            double ax = (segment.first.x - p.x) * c, ay = segment.first.y - p.y;
            double bx = (segment.second.x - p.x) * c, by = segment.second.y - p.y;
            double dx = bx - ax, dy = by - ay;
            double length = dx * dx + dy * dy;
            double t = length > 0.0 ? std::clamp(-(ax * dx + ay * dy) / length, 0.0, 1.0) : 0.0;
            nearest = std::min(nearest, std::hypot(ax + dx * t, ay + dy * t));
        }
        return nearest;
    };
    int doubled = 0;
    int wet = 0;
    for (int l = 0; l < Admin1Lines::LINE_COUNT; l++)
    {
        for (uint32_t i = Admin1Lines::LINE_START[l]; i + 1 < Admin1Lines::LINE_START[l + 1]; i++)
        {
            Point a{Admin1Lines::POINTS[2 * i] / 100.0, Admin1Lines::POINTS[2 * i + 1] / 100.0};
            Point b{Admin1Lines::POINTS[2 * i + 2] / 100.0, Admin1Lines::POINTS[2 * i + 3] / 100.0};
            Point middle{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
            if (toBorder(a) < 0.3 && toBorder(middle) < 0.3 && toBorder(b) < 0.3) doubled++;
            if (isLake(LatLon{middle.y, middle.x})) wet++;
        }
    }
    CHECK_EQ(doubled, 0);
    CHECK_EQ(wet, 0);
    for (int l = 0; l < Admin1Lines::LINE_COUNT; l++) CHECK(Admin1Lines::LINE_START[l + 1] - Admin1Lines::LINE_START[l] >= 2);

    // Every country has its lines: points in each one's middle. Among
    // Canada's is British Columbia's with the Northwest Territories, along
    // the 60th parallel from 120 W to the Rockies, which the 1:50m set
    // lacks.
    const Box middles[] = {
        {30.0, 45.0, -110.0, -85.0},    // the United States
        {50.0, 60.0, -115.0, -75.0},    // Canada
        {17.0, 28.0, -110.0, -90.0},    // Mexico
        {50.0, 70.0, 40.0, 140.0},      // Russia
        {25.0, 40.0, 100.0, 120.0},     // China
        {12.0, 28.0, 72.0, 86.0},       // India
        {-30.0, -5.0, -60.0, -38.0},    // Brazil
        {-45.0, -25.0, -70.0, -58.0},   // Argentina
        {-38.0, -15.0, 125.0, 150.0},   // Australia
    };
    for (const Box& box : middles)
    {
        int points = 0;
        for (int i = 0; i < Admin1Lines::POINT_COUNT; i++)
        {
            points += inBox(box, Admin1Lines::POINTS[2 * i + 1] / Admin1Lines::UNITS_PER_DEGREE,
                            Admin1Lines::POINTS[2 * i] / Admin1Lines::UNITS_PER_DEGREE);
        }
        CHECK(points >= 10);
    }
    int alongSixty = 0;
    for (int i = 0; i < Admin1Lines::POINT_COUNT; i++)
    {
        double lon = Admin1Lines::POINTS[2 * i] / Admin1Lines::UNITS_PER_DEGREE;
        double lat = Admin1Lines::POINTS[2 * i + 1] / Admin1Lines::UNITS_PER_DEGREE;
        if (std::fabs(lat - 60.0) < 0.05 && lon > -123.5 && lon < -120.5) alongSixty++;
    }
    CHECK(alongSixty >= 1);

    // Over Kansas there are dozens, all on the near side; close over the
    // middle of the Pacific none.
    Window window{320.0, 320.0};
    View kansas{lookingAt(toVector(LatLon{39.0, -98.0})), 148.0};
    std::vector<Outline> lines = admin1Lines(kansas, window);
    CHECK(lines.size() > 60);
    for (const Outline& line : lines)
    {
        for (const Point& p : line) CHECK(std::hypot(p.x - 160.0, p.y - 160.0) <= 148.0 + 1e-6);
    }
    View pacific{lookingAt(toVector(LatLon{0.0, -150.0})), 1184.0};
    CHECK(admin1Lines(pacific, window).empty());
}

std::int64_t utc(int year, int month, int day, int hour, int minute)
{
    // Days from 1970 by the civil calendar (Howard Hinnant's days_from_civil).
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = (std::int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60;
}

void testSubsolar()
{
    CHECK_EQ(utc(2026, 10, 21, 22, 36), (std::int64_t)1792622160);

    // At the solstices the sun is over the tropics, and at noon UTC near
    // Greenwich, off it by the equation of time.
    LatLon june = subsolarPoint(utc(2026, 6, 21, 12, 0));
    CHECK(near(june.lat, 23.44, 0.03));
    CHECK(near(june.lon, 0.46, 0.3));
    LatLon december = subsolarPoint(utc(2026, 12, 21, 12, 0));
    CHECK(near(december.lat, -23.43, 0.03));
    CHECK(near(december.lon, -0.48, 0.3));

    // At the March equinox it crosses the equator.
    CHECK(near(subsolarPoint(utc(2026, 3, 20, 14, 46)).lat, 0.0, 0.05));

    // Early November the sun runs 16 minutes fast, so at noon UTC it is
    // already 4 degrees west of Greenwich.
    CHECK(near(subsolarPoint(utc(2026, 11, 3, 12, 0)).lon, -4.1, 0.2));

    // It goes west 15 degrees an hour, and wraps at the date line.
    LatLon mock = subsolarPoint(utc(2026, 10, 21, 22, 36));
    CHECK(near(mock.lat, -10.95, 0.03));
    CHECK(near(mock.lon, -162.86, 0.1));
    LatLon hourLater = subsolarPoint(utc(2026, 10, 21, 23, 36));
    CHECK(near(hourLater.lon, mock.lon - 15.04, 0.1));
    LatLon twoHoursLater = subsolarPoint(utc(2026, 10, 22, 0, 36));
    CHECK(near(twoHoursLater.lon, 360.0 + mock.lon - 30.08, 0.1));
}

void testDaylight()
{
    const double PI = std::acos(-1.0);
    auto height = [PI](double degrees) { return std::sin(degrees * PI / 180.0); };
    CHECK(near(daylightShare(1.0), 1.0, 1e-12));
    CHECK(near(daylightShare(height(3.0)), 1.0, 1e-12));
    CHECK(near(daylightShare(height(-12.0)), 0.0, 1e-12));
    CHECK(near(daylightShare(-1.0), 0.0, 1e-12));
    double last = -1.0;
    bool rising = true;
    for (double a = -20.0; a <= 10.0; a += 0.25)
    {
        double d = daylightShare(height(a));
        if (d < last) rising = false;
        last = d;
    }
    CHECK(rising);
    CHECK(daylightShare(height(-4.5)) > 0.4 && daylightShare(height(-4.5)) < 0.6);

    // Noon overhead: as it was, but for the rim.
    Shade noon = shadeAt(1.0, 1.0);
    CHECK(near(noon.keep, 1.0, 1e-12) && near(noon.night, 0.0, 1e-12));
    Shade rimNoon = shadeAt(1.0, 0.0);
    CHECK(near(rimNoon.keep, 0.88, 1e-12));
    // Low sun by day, Lambert-lit a little darker.
    CHECK(near(shadeAt(0.5, 1.0).keep, 0.965, 1e-12));
    // Midnight: 0.30 of the colour and 0.55 of the navy.
    Shade midnight = shadeAt(-1.0, 1.0);
    CHECK(near(midnight.keep, 0.30, 1e-12) && near(midnight.night, 0.55, 1e-12));
}

// The overlay's pixel laid over a colour channel, as a drawing library
// blends it.
double blended(const Overlay& overlay, int x, int y, int channel, double under)
{
    size_t at = (size_t)y * (size_t)overlay.width + (size_t)x;
    double a = overlay.alpha[at] / 255.0;
    return overlay.rgb[3 * at + (size_t)channel] * a + under * (1.0 - a);
}

void testOverlay()
{
    Window window{100.0, 80.0};
    LatLon sun{10.0, 40.0};
    Vec3 s = toVector(sun);
    Overlay overlay;

    // Looking down on the sun, the middle is lit full and nothing shades it.
    lightOverlay(View{lookingAt(s), 30.0}, window, sun, 1.0, overlay);
    CHECK_EQ(overlay.width, 100);
    CHECK_EQ(overlay.height, 80);
    CHECK_EQ(overlay.rgb.size(), (size_t)100 * 80 * 3);
    CHECK_EQ(overlay.alpha.size(), (size_t)100 * 80);
    CHECK(overlay.alpha[40 * 100 + 50] <= 1);
    // Off the globe, nothing at all.
    CHECK_EQ((int)overlay.alpha[0], 0);
    CHECK_EQ((int)overlay.alpha[40 * 100 + 5], 0);
    // Towards the rim, darker: limb darkening and a lower sun.
    CHECK(overlay.alpha[40 * 100 + 78] > overlay.alpha[40 * 100 + 60]);

    // Looking down on midnight, the middle is 0.30 of the colour and 0.55
    // of the navy, whatever the colour, sea or land.
    Vec3 midnight{-s.x, -s.y, -s.z};
    lightOverlay(View{lookingAt(midnight), 30.0}, window, sun, 1.0, overlay);
    for (double under : {168.0, 247.0, 0.0, 255.0})
    {
        CHECK(near(blended(overlay, 50, 40, 0, under), under * 0.30 + NIGHT.r * 0.55, 1.5));
        CHECK(near(blended(overlay, 50, 40, 2, under), under * 0.30 + NIGHT.b * 0.55, 1.5));
    }

    // On the greyline, between the two.
    Vec3 dusk = toVector(LatLon{sun.lat - 90.0 - 4.5, sun.lon});
    lightOverlay(View{lookingAt(dusk), 30.0}, window, sun, 1.0, overlay);
    double grey = blended(overlay, 50, 40, 0, 247.0);
    CHECK(grey < 0.9 * 247.0 && grey > 247.0 * 0.30 + NIGHT.r * 0.55 + 10.0);

    // Every pixel, blended, is what shadeAt says, on a doubled-up screen
    // too: the rim aside, where the pixel is only partly the globe's.
    View view{lookingAt(toVector(LatLon{40.0, -100.0}), 0.3), 36.0};
    lightOverlay(view, window, sun, 2.0, overlay);
    CHECK_EQ(overlay.width, 200);
    int compared = 0;
    int wrong = 0;
    for (int py = 0; py < overlay.height; py += 3)
    {
        for (int px = 0; px < overlay.width; px += 3)
        {
            double x = ((px + 0.5) / 2.0 - 50.0) / 36.0;
            double y = (40.0 - (py + 0.5) / 2.0) / 36.0;
            double r2 = x * x + y * y;
            if (r2 > 0.95) continue;
            Vec3 face{x, y, std::sqrt(1.0 - r2)};
            Shade shade = shadeAt(dot(toEarth(view.attitude, face), s), face.z);
            compared++;
            if (!near(blended(overlay, px, py, 1, 212.0), 212.0 * shade.keep + NIGHT.g * shade.night, 1.5)) wrong++;
        }
    }
    CHECK(compared > 500);
    CHECK_EQ(wrong, 0);

    // The rim is smoothed: some pixels part covered.
    int partial = 0;
    for (unsigned char a : overlay.alpha) partial += a > 0 && a < 60;
    CHECK(partial > 10);

    // Nothing to draw into, nothing drawn.
    lightOverlay(view, Window{0.0, 0.0}, sun, 1.0, overlay);
    CHECK(overlay.alpha.empty() && overlay.rgb.empty());
}

void testLightingTime()
{
    const std::int64_t mock = utc(2026, 10, 21, 22, 36);
    CHECK_EQ(lightingTime(mock, -7 * 3600, "PDT"), std::string("22:36 UTC / 15:36 PDT"));
    CHECK_EQ(lightingTime(mock, -7 * 3600, "Pacific Daylight Time"), std::string("22:36 UTC / 15:36 PDT"));
    CHECK_EQ(lightingTime(mock, 0, "UTC"), std::string("22:36 UTC"));
    CHECK_EQ(lightingTime(mock, 0, "GMT"), std::string("22:36 UTC"));
    CHECK_EQ(lightingTime(mock, 0, "Coordinated Universal Time"), std::string("22:36 UTC"));
    CHECK_EQ(lightingTime(mock, 3600, "BST"), std::string("22:36 UTC / 23:36 BST"));
    CHECK_EQ(lightingTime(mock, 9 * 3600, "JST"), std::string("22:36 UTC / 07:36 JST"));
    CHECK_EQ(lightingTime(mock, 5 * 3600 + 1800, "IST"), std::string("22:36 UTC / 04:06 IST"));
    CHECK_EQ(lightingTime(mock, -7 * 3600, ""), std::string("22:36 UTC / 15:36 local"));
    CHECK_EQ(lightingTime(utc(1969, 12, 31, 23, 59), 0, "UTC"), std::string("23:59 UTC"));

    CHECK_EQ(zoneAbbreviation("W. Europe Daylight Time"), std::string("WEDT"));
    CHECK_EQ(zoneAbbreviation("+03"), std::string("+03"));

    std::tm local{};
    std::tm universal{};
    local.tm_year = universal.tm_year = 126;
    local.tm_yday = universal.tm_yday = 293;
    local.tm_hour = 15;
    universal.tm_hour = 22;
    local.tm_min = universal.tm_min = 36;
    CHECK_EQ(utcOffsetSeconds(local, universal), -7 * 3600);
    // West of Greenwich on New Year's Eve, UTC already in the new year.
    local.tm_yday = 364;
    local.tm_hour = 16;
    local.tm_min = 0;
    universal.tm_year = 127;
    universal.tm_yday = 0;
    universal.tm_hour = 0;
    universal.tm_min = 0;
    CHECK_EQ(utcOffsetSeconds(local, universal), -8 * 3600);
    // East of it, the other way round.
    CHECK_EQ(utcOffsetSeconds(universal, local), 8 * 3600);
}
} // namespace

int main()
{
    testLocators();
    testDistanceAndBearing();
    testCaption();
    testLand();
    testAttitude();
    testGreatCircle();
    testFraming();
    testGlobusFraming();
    testRolling();
    testScaling();
    testSpinning();
    testOutlines();
    testBorders();
    testLakes();
    testBordersStopAtLakes();
    testCoastsAreNotBorders();
    testAdmin1();
    testSubsolar();
    testDaylight();
    testOverlay();
    testLightingTime();
    return ragtime_test::finish();
}
