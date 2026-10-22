// The sums behind Globus, the globe beside the messages: where a Maidenhead
// locator is, how far and which way, which way up the globe shows a path
// best, how it rolls there, and the outlines of its land, borders, state
// and province lines and graticule, as the window shows them.
//
// Ported from Glissando's map ball (app/src/gui/glissando/Globe.*), which
// Jeff relicenses as MIT here. Nothing here depends on wxWidgets: the app's
// GlobusView draws what this works out. Sun.h lights it.
//
// Earth coordinates are unit vectors with x through 0 N 0 E, y through
// 0 N 90 E and z through the north pole. A view looks down on the globe
// with x to the right, y up and z out of the screen at the viewer, so a
// point of the earth is on the near side when its view z is positive.
#ifndef RAGTIME_GLOBUS_GLOBE_H
#define RAGTIME_GLOBUS_GLOBE_H

#include <string>
#include <vector>

namespace ragtime::globus
{

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LatLon
{
    double lat = 0.0;   // degrees north
    double lon = 0.0;   // degrees east
};

Vec3 toVector(const LatLon& position);
LatLon toLatLon(const Vec3& v);
double dot(const Vec3& a, const Vec3& b);

// The centre of a Maidenhead locator of four or six characters, in any
// case. False for anything else.
bool locatorCentre(const std::string& locator, LatLon& centreOut);

// The locator tidied: fields upper case, subsquare lower case ("CN87ux").
// Empty unless it is one.
std::string tidyLocator(const std::string& locator);

// Great circle distance in kilometres, on a sphere of the earth's mean
// radius, and the initial bearing from one point to the other in degrees
// clockwise from north, 0 to 360.
double distanceKm(const LatLon& from, const LatLon& to);
double bearingDegrees(const LatLon& from, const LatLon& to);

// A whole number with thousands separated by commas: "3,951".
std::string withThousands(long value);

// What the caption under the globe says: the station's callsign and grid
// square, and from home its distance and bearing, as in
// "W1AW · FN31 · 3,951 km · 81°". Each part is left out when it isn't
// known: without a home locator, no distance; without a station locator,
// just the callsign; without a station at all, our own square. A square
// kept from an earlier contact, not current, says so after it:
// "FN31 (last contact)".
std::string caption(const std::string& homeLocator, const std::string& callsign,
                    const std::string& stationLocator, bool current = true);

// Which way the globe is turned: the earth directions that point right, up
// and out of the screen.
struct Attitude
{
    Vec3 right{0.0, 1.0, 0.0};
    Vec3 up{0.0, 0.0, 1.0};
    Vec3 out{1.0, 0.0, 0.0};
};

Vec3 toView(const Attitude& attitude, const Vec3& earth);
Vec3 toEarth(const Attitude& attitude, const Vec3& view);

// Looking straight down on centre with north up, then the picture turned
// clockwise by roll radians.
Attitude lookingAt(const Vec3& centre, double roll = 0.0);

// The angle between two attitudes, in radians: how far the globe has to
// roll.
double angleBetween(const Attitude& a, const Attitude& b);

// The shorter great circle from a to b in segments steps: segments + 1
// points, a first.
std::vector<Vec3> greatCircle(const Vec3& a, const Vec3& b, int segments);

// The window the globe is seen through, in pixels. The globe is centred in
// it.
struct Window
{
    double width = 320.0;
    double height = 320.0;
};

// How the globe is seen: which way it is turned, and how big it is, its
// radius in pixels. The bigger, the closer the view.
struct View
{
    Attitude attitude;
    double radius = 148.0;
};

// How close the globe comes: when framing a path, the radius at most
// nearest and at least farthest; showing one place, standard. The mouse
// wheel goes from farthest to closest.
struct Zoom
{
    double nearest = 148.0;
    double farthest = 148.0;
    double standard = 148.0;
    double closest = 1184.0;
};

// Globus's zoom for a window: framed, the whole globe with a margin round
// it, as the mock-ups show it, so the greyline always shows; the wheel
// comes up to eight times closer.
Zoom wholeGlobe(const Window& window);

// A point in the window, in pixels from its top left corner, and the point
// of the earth a view puts there (view z positive on the near side).
struct Point
{
    double x = 0.0;
    double y = 0.0;
};
Point onScreen(const View& view, const Window& window, const Vec3& earth);

// The view that best shows the path from home to a station: as close as
// shows all of it, north up if that is nearly as close as any turn of the
// picture, else turned. The globe centres a little to the equator side of
// the path's midpoint, so the path curls over the globe rather than
// running straight through the middle.
View framePath(const LatLon& home, const LatLon& station, const Window& window, const Zoom& zoom);

// The attitude that best shows the path at a given radius: north up if
// that shows the whole path, or as much of it as any turn does; otherwise
// the smallest turn, either way, that shows the most.
Attitude framePathAt(const LatLon& home, const LatLon& station, const Window& window, double radius);

// How many of the path's points the window shows.
int pointsInView(const View& view, const std::vector<Vec3>& path, const Window& window);

// The magnet in the globe for a path: the direction from its southern end
// to its northern end, through the globe. Ends at the same latitude go from
// home to the station.
Vec3 magnetFor(const LatLon& home, const LatLon& station);

// A heavy globe floating in something thick, with a magnet in it.
//
// Left to itself it coasts: spun, it keeps turning, the fluid slowing it
// gently until it is too slow to stir the fluid at all and stops.
//
// Sent to a view, a field comes on that lines the magnet up with where the
// view has it, and the middle of the view floats up to the window, so the
// globe tumbles round into the view and settles there. The field also
// brakes the magnet as it turns, as a magnet is braked moving past copper,
// so a spinning globe soon gives up its spin. Sent far, it backs away as it
// rolls, so the roll can be followed, and comes in again as it arrives.
//
// Held by the mouse, the point of the globe under the pointer follows the
// pointer, and anything else it was doing stops almost at once.
class Roller
{
public:
    Roller();

    // How wide, in pixels from the window's middle to its side, the globe
    // backs off to keep what is left of a long roll in sight. Zero, the
    // start, does not back off.
    void setBackOff(double halfWidth) { backOff_ = halfWidth; }

    // Straight there, still, the field on.
    void jump(const View& view);

    // The field on, to draw the globe to a view; with the magnet along an
    // earth direction, or else the view's up.
    void rollTo(const View& view);
    void rollTo(const View& view, const Vec3& magnet);

    // The field off: the globe coasts on as it was going.
    void coast();

    // Nearer or farther, whatever it is doing.
    void zoomTo(double radius);

    // Bigger or smaller at once by a factor, where it is and where it is
    // going alike, as when the window it is seen through is resized.
    void scaleBy(double factor);

    // The mouse pressed at a point of the window, in pixels right and up
    // from its middle: whether the globe is there to hold. While held, the
    // point of the globe first pressed on follows the pointer; let go, the
    // globe keeps the spin the pointer gave it.
    bool grab(double x, double y);
    void dragTo(double x, double y);
    void letGo();
    bool held() const { return held_; }

    // Advances it by seconds. Returns whether the globe is still moving, or
    // held, afterwards.
    bool step(double seconds);

    View view() const;
    // Where it is going: the view the field draws it to, or where it is
    // when coasting; at the radius it is zooming to.
    View target() const;
    bool fieldOn() const { return fieldOn_; }
    bool moving() const { return moving_; }

private:
    struct Quaternion
    {
        double w = 1.0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };
    static Quaternion fromAttitude(const Attitude& attitude);
    static Attitude toAttitude(const Quaternion& q);

    // The point of the globe under the pointer, in view axes: on the rim
    // when the pointer is off the globe.
    Vec3 underPointer(double radius) const;

    Quaternion current_;
    Quaternion target_;
    Vec3 spin_;             // radians a second, about view axes
    double logRadius_;
    double targetLogRadius_;
    double zoomSpeed_;      // of logRadius_, a second
    double backOff_;

    bool fieldOn_;
    double field_;          // how far the field has come on, 0 to 1
    Vec3 magnet_;           // in the earth
    Vec3 floats_;           // the earth point that floats up to the window

    bool held_;
    Vec3 grabbed_;          // the point held, in the earth
    double pointerX_;
    double pointerY_;

    bool moving_;
};

// A run of points in the window, in order.
using Outline = std::vector<Point>;

// The land the window shows, as outlines to fill together by the non-zero
// winding rule: a hole runs the other way round from the land around it.
// Where land goes round the back of the globe its outline follows the rim.
std::vector<Outline> landOutlines(const View& view, const Window& window);

// The big lakes the window shows, outlined in the same way, to fill in the
// sea's colour over the land: the 1:110m land fills them in, Michigan's
// Lower Peninsula joined to Wisconsin.
std::vector<Outline> lakeOutlines(const View& view, const Window& window);

// The coasts and lake shores on the near side of the globe, as lines to
// stroke: the land's own edges, without the stretches of rim landOutlines
// adds to close them, and without the edges two countries share, which are
// borders (borderLines).
std::vector<Outline> coastLines(const View& view, const Window& window);

// The borders between countries on the near side of the globe, smoothed,
// as lines to stroke.
std::vector<Outline> borderLines(const View& view, const Window& window);

// The state and province lines on the near side of the globe, as lines to
// stroke: for the United States, Canada, Mexico, Russia, China, India,
// Brazil, Argentina and Australia.
std::vector<Outline> admin1Lines(const View& view, const Window& window);

// The parallels and meridians every stepDegrees on the near side of the
// globe, as lines to stroke: the parallels between the poles, and the
// meridians from pole to pole.
std::vector<Outline> graticule(const View& view, const Window& window, int stepDegrees = 30);

// Whether the outlines have land at a position, counting the lakes as land
// as the 1:110m countries do; and whether a big lake is there.
bool isLand(const LatLon& position);
bool isLake(const LatLon& position);

} // namespace ragtime::globus

#endif // RAGTIME_GLOBUS_GLOBE_H
