#include "GlobusView.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/time.h>

using namespace ragtime::globus;

namespace
{

// Frames while the globe rolls, and how long the path takes to curl out
// from our dot to the station.
constexpr int FRAME_MILLISECONDS = 20;
constexpr double PATH_SECONDS = 1.6;
constexpr int PATH_SEGMENTS = 96;

// Each notch of the mouse wheel comes this much closer or backs this much
// off.
constexpr double WHEEL_STEP = 1.25;

// Where the globe looks before it knows any grid square: the North
// Atlantic, the Americas to one side and Europe and Africa to the other.
const LatLon NOWHERE_YET{30.0, -40.0};

// The theme (docs/MOCKUPS.md): light blue oceans, near-white land, dark
// blue coasts and borders, the state and province lines the same blue at
// about half opacity and thinner, and a very faint graticule every 30°.
const wxColour OCEAN(0xA8, 0xD4, 0xF2);
const wxColour LAND(0xF7, 0xF5, 0xEE);
const wxColour COAST(0x14, 0x30, 0x68);
const wxColour BORDER(0x14, 0x30, 0x68, 220);
const wxColour ADMIN1(0x14, 0x30, 0x68, 110);
const wxColour GRATICULE(255, 255, 255, 90);
constexpr double COAST_WIDTH = 1.2;
constexpr double BORDER_WIDTH = 0.9;
constexpr double ADMIN1_WIDTH = 0.6;
constexpr double GRATICULE_WIDTH = 0.8;

// The path a red great circle; home a navy dot, the picked station a red
// one, the other stations smaller red dots, all with white rims.
const wxColour PATH(0xc8, 0x28, 0x1e);
const wxColour HOME(0x14, 0x30, 0x68);
const wxColour STATION(0xc8, 0x28, 0x1e);
constexpr double PATH_WIDTH = 2.0;
constexpr double END_RADIUS = 4.5;
constexpr double DOT_RADIUS = 3.0;
constexpr unsigned char STALE_ALPHA = 110;

// Eases the path out quickly and in slowly, like the globe.
double eased(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

wxPoint2DDouble at(const Point& p)
{
    return wxPoint2DDouble(p.x, p.y);
}

void addLines(wxGraphicsPath& path, const std::vector<Outline>& lines)
{
    for (const Outline& line : lines)
    {
        path.MoveToPoint(at(line.front()));
        for (size_t i = 1; i < line.size(); i++) path.AddLineToPoint(at(line[i]));
    }
}

void stroke(wxGraphicsContext* gc, const std::vector<Outline>& lines, const wxColour& colour, double width)
{
    wxGraphicsPath path = gc->CreatePath();
    addLines(path, lines);
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(colour, width).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
    gc->StrokePath(path);
}

void drawDot(wxGraphicsContext* gc, const wxPoint2DDouble& p, const wxColour& fill, double radius)
{
    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255, 255, 255, fill.Alpha()), 1.2)));
    gc->SetBrush(wxBrush(fill));
    gc->DrawEllipse(p.m_x - radius, p.m_y - radius, 2.0 * radius, 2.0 * radius);
}

bool sameView(const View& a, const View& b)
{
    return a.radius == b.radius && a.attitude.out.x == b.attitude.out.x && a.attitude.out.y == b.attitude.out.y
           && a.attitude.out.z == b.attitude.out.z && a.attitude.up.x == b.attitude.up.x
           && a.attitude.up.y == b.attitude.up.y && a.attitude.up.z == b.attitude.up.z;
}

} // namespace

GlobusView::GlobusView(wxWindow* parent, wxWindowID id)
    : wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
    , timer_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    window_.width = 320.0;
    window_.height = 320.0;
    zoom_ = wholeGlobe(window_);
    roller_.setBackOff(window_.width / 2.0);
    roller_.jump(fitted());
    setTime((std::int64_t)std::time(nullptr));

    SetToolTip("Drag the globe to spin it and hold it to stop it; turn the mouse wheel over it to "
               "zoom; double-click to fit the path again.");
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &GlobusView::OnPaint, this);
    Bind(wxEVT_SIZE, &GlobusView::OnSize, this);
    Bind(wxEVT_TIMER, &GlobusView::OnTimer, this);
    Bind(wxEVT_MOUSEWHEEL, &GlobusView::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DCLICK, &GlobusView::OnDoubleClick, this);
    Bind(wxEVT_LEFT_DOWN, &GlobusView::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &GlobusView::OnMouseUp, this);
    Bind(wxEVT_MOTION, &GlobusView::OnMouseMove, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &GlobusView::OnCaptureLost, this);
}

wxSize GlobusView::DoGetBestClientSize() const
{
    // The side panes are 280 to 340 pixels wide (docs/MOCKUPS.md).
    return wxSize(300, 300);
}

View GlobusView::fitted() const
{
    if (showsPath()) return framePath(homeAt_, stationAt_, window_, zoom_);
    View view;
    view.radius = zoom_.standard;
    view.attitude = lookingAt(toVector(haveStation_ ? stationAt_ : haveHome_ ? homeAt_ : NOWHERE_YET));
    return view;
}

Vec3 GlobusView::magnet() const
{
    if (showsPath()) return magnetFor(homeAt_, stationAt_);
    return fitted().attitude.up;
}

void GlobusView::rollTo(const View& view)
{
    roller_.rollTo(view, magnet());
    wake();
}

void GlobusView::wake()
{
    if (timer_.IsRunning()) return;
    lastTickMs_ = wxGetLocalTimeMillis().GetValue();
    timer_.Start(FRAME_MILLISECONDS);
}

void GlobusView::pointer(const wxMouseEvent& event, double& x, double& y) const
{
    x = event.GetX() - window_.width / 2.0;
    y = window_.height / 2.0 - event.GetY();
}

void GlobusView::setLocators(const std::string& home, const std::string& station, bool stationCurrent)
{
    if (home == home_ && station == station_ && stationCurrent == stationCurrent_) return;
    bool moved = home != home_ || station != station_;
    bool stationChanged = station != station_;
    home_ = home;
    station_ = station;
    stationCurrent_ = stationCurrent;
    haveHome_ = locatorCentre(home, homeAt_);
    haveStation_ = locatorCentre(station, stationAt_);
    if (!moved)
    {
        // Only the square's standing changed: it dims or brightens.
        Refresh();
        return;
    }

    if (!placed_ && (haveHome_ || haveStation_))
    {
        // The first square it is given, it starts at: no rolling to get
        // there.
        roller_.jump(fitted());
        placed_ = true;
        pathDrawn_ = 1.0;
    }
    else if (stationChanged && station.empty())
    {
        // The station is gone, picked off in the station list: the path
        // with it, and the globe is let go where it is.
        roller_.coast();
        wake();
    }
    else
    {
        if (stationChanged) pathDrawn_ = 0.0;
        rollTo(fitted());
    }
    Refresh();
}

void GlobusView::setStations(const std::vector<std::pair<std::string, bool>>& stations)
{
    if (stations == stations_) return;
    stations_ = stations;
    dots_.clear();
    for (const auto& station : stations)
    {
        LatLon where;
        if (locatorCentre(station.first, where)) dots_.push_back({where, station.second});
    }
    Refresh();
}

void GlobusView::setTime(std::int64_t unixSeconds)
{
    sun_ = subsolarPoint(unixSeconds);
    Refresh();
}

void GlobusView::OnSize(wxSizeEvent& event)
{
    wxSize size = GetClientSize();
    if (size.x > 0 && size.y > 0)
    {
        Zoom before = zoom_;
        window_.width = size.x;
        window_.height = size.y;
        zoom_ = wholeGlobe(window_);
        roller_.setBackOff(window_.width / 2.0);
        roller_.scaleBy(zoom_.standard / before.standard);
    }
    Refresh();
    event.Skip();
}

void GlobusView::OnTimer(wxTimerEvent&)
{
    long long now = wxGetLocalTimeMillis().GetValue();
    double seconds = std::clamp((now - lastTickMs_) / 1000.0, 0.0, 0.1);
    lastTickMs_ = now;

    roller_.step(seconds);
    pathDrawn_ = std::min(1.0, pathDrawn_ + seconds / PATH_SECONDS);
    if (!roller_.moving() && pathDrawn_ >= 1.0) timer_.Stop();
    Refresh();
}

void GlobusView::OnMouseWheel(wxMouseEvent& event)
{
    if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || event.GetWheelDelta() == 0) return;

    // From where it is going, so quick turns of the wheel add up.
    double notches = (double)event.GetWheelRotation() / event.GetWheelDelta();
    roller_.zoomTo(std::clamp(roller_.target().radius * std::pow(WHEEL_STEP, notches), zoom_.farthest, zoom_.closest));
    wake();
}

void GlobusView::OnDoubleClick(wxMouseEvent&)
{
    rollTo(fitted());
}

void GlobusView::OnMouseDown(wxMouseEvent& event)
{
    double x = 0.0, y = 0.0;
    pointer(event, x, y);
    if (!roller_.grab(x, y)) return;

    // With nothing but our own square to show, nothing draws the globe
    // back: let go, it is free to spin.
    if (!showsPath()) roller_.coast();
    if (!HasCapture()) CaptureMouse();
    wake();
}

void GlobusView::OnMouseUp(wxMouseEvent&)
{
    if (HasCapture()) ReleaseMouse();
    roller_.letGo();
}

void GlobusView::OnMouseMove(wxMouseEvent& event)
{
    if (!roller_.held()) return;
    double x = 0.0, y = 0.0;
    pointer(event, x, y);
    roller_.dragTo(x, y);
}

void GlobusView::OnCaptureLost(wxMouseCaptureLostEvent&)
{
    roller_.letGo();
}

const wxBitmap& GlobusView::lighting(const View& view, double scale)
{
    if (lightingBitmap_.IsOk() && sameView(view, lightingView_) && scale == lightingScale_
        && sun_.lat == lightingSun_.lat && sun_.lon == lightingSun_.lon && window_.width == lightingWindow_.width
        && window_.height == lightingWindow_.height)
    {
        return lightingBitmap_;
    }
    lightOverlay(view, window_, sun_, scale, overlay_);
    lightingView_ = view;
    lightingScale_ = scale;
    lightingSun_ = sun_;
    lightingWindow_ = window_;
    if (overlay_.width <= 0 || overlay_.height <= 0)
    {
        lightingBitmap_ = wxBitmap();
        return lightingBitmap_;
    }
    // wxImage takes the rows of bytes over and frees them itself.
    unsigned char* rgb = (unsigned char*)malloc(overlay_.rgb.size());
    unsigned char* alpha = (unsigned char*)malloc(overlay_.alpha.size());
    std::copy(overlay_.rgb.begin(), overlay_.rgb.end(), rgb);
    std::copy(overlay_.alpha.begin(), overlay_.alpha.end(), alpha);
    wxImage image(overlay_.width, overlay_.height, rgb, alpha, false);
    lightingBitmap_ = wxBitmap(image);
    return lightingBitmap_;
}

void GlobusView::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc || window_.width <= 0 || window_.height <= 0) return;

    const View view = roller_.view();
    const double cx = window_.width / 2.0;
    const double cy = window_.height / 2.0;
    const double radius = view.radius;

    // The sea, then all the land in one go, so where two countries meet
    // there is no seam.
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(OCEAN));
    gc->DrawEllipse(cx - radius, cy - radius, 2.0 * radius, 2.0 * radius);
    wxGraphicsPath land = gc->CreatePath();
    for (const Outline& outline : landOutlines(view, window_))
    {
        land.MoveToPoint(at(outline.front()));
        for (size_t i = 1; i < outline.size(); i++) land.AddLineToPoint(at(outline[i]));
        land.CloseSubpath();
    }
    gc->SetBrush(wxBrush(LAND));
    gc->FillPath(land, wxWINDING_RULE);

    // The sunlight over both, then the lines, which stay crisp at night.
    const wxBitmap& light = lighting(view, GetContentScaleFactor());
    if (light.IsOk()) gc->DrawBitmap(light, 0.0, 0.0, window_.width, window_.height);

    stroke(gc.get(), graticule(view, window_, 30), GRATICULE, GRATICULE_WIDTH);
    stroke(gc.get(), admin1Lines(view, window_), ADMIN1, ADMIN1_WIDTH);
    stroke(gc.get(), borderLines(view, window_), BORDER, BORDER_WIDTH);
    stroke(gc.get(), coastLines(view, window_), COAST, COAST_WIDTH);

    const Vec3 home = toVector(homeAt_);
    const Vec3 station = toVector(stationAt_);
    if (showsPath() && pathDrawn_ > 0.0)
    {
        // The near side of the path, as far as it has curled out so far.
        std::vector<Vec3> path = greatCircle(home, station, PATH_SEGMENTS);
        double reach = eased(pathDrawn_) * PATH_SEGMENTS;
        std::vector<std::vector<wxPoint2DDouble>> runs(1);
        for (int i = 0; i <= PATH_SEGMENTS; i++)
        {
            Vec3 p = path[(size_t)i];
            if (i > reach)
            {
                // The last piece, part of a segment.
                double f = reach - std::floor(reach);
                const Vec3& a = path[(size_t)i - 1];
                const Vec3& b = path[(size_t)i];
                p = Vec3{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
            }
            if (toView(view.attitude, p).z > 0.0) runs.back().push_back(at(onScreen(view, window_, p)));
            else if (!runs.back().empty()) runs.emplace_back();
            if (i > reach) break;
        }
        wxColour red = PATH;
        if (!stationCurrent_) red = wxColour(red.Red(), red.Green(), red.Blue(), 150);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(red, PATH_WIDTH).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
        for (const auto& run : runs)
        {
            if (run.size() >= 2) gc->StrokeLines(run.size(), run.data());
        }
    }

    // The other stations, then the station picked and home on top.
    for (const auto& dot : dots_)
    {
        Vec3 v = toVector(dot.first);
        if (toView(view.attitude, v).z <= 0.0) continue;
        wxColour red = STATION;
        if (!dot.second) red = wxColour(red.Red(), red.Green(), red.Blue(), STALE_ALPHA);
        drawDot(gc.get(), at(onScreen(view, window_, v)), red, DOT_RADIUS);
    }
    if (haveStation_ && toView(view.attitude, station).z > 0.0)
    {
        wxColour red = STATION;
        if (!stationCurrent_) red = wxColour(red.Red(), red.Green(), red.Blue(), 170);
        drawDot(gc.get(), at(onScreen(view, window_, station)), red, END_RADIUS);
    }
    if (haveHome_ && toView(view.attitude, home).z > 0.0)
    {
        drawDot(gc.get(), at(onScreen(view, window_, home)), HOME, END_RADIUS);
    }
}
