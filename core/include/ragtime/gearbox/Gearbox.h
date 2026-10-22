// The gearshift: gear ladders built from the Submode Map, and Auto's pick
// from the path's S/N. docs/GEARBOX.md is the spec;
// tools/gearbox/gearbox_ladder.py is the reference the ladders must match.
#ifndef RAGTIME_GEARBOX_GEARBOX_H
#define RAGTIME_GEARBOX_GEARBOX_H

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/gearbox/Submodes.h"

namespace ragtime::gearbox
{

// The width chips, narrowest to widest: the map's four groups. A width is a
// cap, so 2.4 kHz still uses the narrow modes.
enum class Width
{
    Hz200,
    Hz500,
    Hz1200,
    Hz2400,
};

constexpr std::array<Width, 4> ALL_WIDTHS = {Width::Hz200, Width::Hz500, Width::Hz1200,
                                             Width::Hz2400};

// 200, 500, 1200, 2400. 2.4 kHz takes the 2.3 kHz modes too.
int widthCapHz(Width width);
// "200 Hz", "500 Hz", "1.2 kHz", "2.4 kHz".
const char* widthLabel(Width width);
// The command that caps data2g-host at this width: "BW500" or "BW2300"
// (which lifts the cap). Empty for 200 Hz and 1.2 kHz, which the host can't
// take yet (an ask for Andrew); the popup greys them.
const char* widthHostCommand(Width width);
bool hostSupportsWidth(Width width);

constexpr double GEAR_RATIO = 1.8;       // each gear at least this much faster
constexpr double TOP_GEAR_RATIO = 1.3;   // or the fastest mode replaces the top gear
constexpr double AUTO_MARGIN_DB = 3.0;   // Auto's margin below the path's S/N

struct Gear
{
    int number = 0;  // from 1, the most robust
    std::string mode;
    int widthHz = 0;
    Modulation modulation = Modulation::Other;
    double bitsPerSecond = 0.0;
    double threshold = 0.0;  // S/N this mode needs on the ladder's path
};

// The best modes no wider than capHz on a path: those no other mode beats on
// both speed and S/N, most robust first. Modes with no threshold on the path
// are left out.
std::vector<const Submode*> bestModes(const SubmodeTable& table, Path path, int capHz);

// The gears: gear 1 is the most robust best mode, and each next gear the
// most robust best mode at least GEAR_RATIO times faster than the one below.
// The fastest best mode is always the top gear: added if it is at least
// TOP_GEAR_RATIO times faster than the last gear, else in its place.
std::vector<Gear> gearLadder(const SubmodeTable& table, Path path, int capHz);

// Auto's pick: the index of the fastest gear needing no more than
// pathSnr - margin, or 0 (gear 1) when even gear 1 needs more. -1 for an
// empty ladder.
int autoGearIndex(const std::vector<Gear>& ladder, double pathSnr,
                  double margin = AUTO_MARGIN_DB);

// How many dB short a path is for a mode, with Auto's margin: positive when
// short, zero or negative when the mode fits.
double dbShort(double threshold, double pathSnr, double margin = AUTO_MARGIN_DB);

// Seconds to move 1 KB (1024 bytes) at a raw payload rate.
double secondsPerKilobyte(double bitsPerSecond);

// "1.0 s", "26 s", "4 min 16 s".
std::string formatDuration(double seconds);
// "139 bit/s", "8222 bit/s".
std::string formatSpeed(double bitsPerSecond);
// "+1.1 dB", "−3.0 dB" (a real minus sign, UTF-8).
std::string formatDb(double dB);
// The Gear button by Send: "Gear 3 · polar-k96-f4 · 139 bit/s".
std::string gearLabel(const Gear& gear);

// The path's S/N, from a station's last few bursts: their mean in dB.
class PathSnr
{
public:
    static constexpr std::size_t BURSTS = 5;

    void record(double dB);
    void clear() { bursts_.clear(); }
    std::optional<double> average() const;
    std::size_t count() const { return bursts_.size(); }

private:
    std::deque<double> bursts_;
};

// A mode in data2g-host's MODES list, as the gearshift needs it. The host's
// width and speed win over a file's; so do its thresholds, when MODES
// carries them.
struct HostMode
{
    std::string name;
    int widthHz = 0;
    double bitsPerSecond = 0.0;  // payloadBitsPerSecond() of its airtimes
    std::array<std::optional<double>, PATH_COUNT> threshold;
};

// The gearshift's state: width, path, Auto, and the gear in effect. The
// popup draws it and the Gear button shows current().
class Gearshift
{
public:
    // The thresholds table. Without one there are no gears.
    void setTable(SubmodeTable table);
    const SubmodeTable& table() const { return table_; }

    // data2g-host's MODES list: gears use only those modes. Empty until the
    // host has listed them, meaning every mode in the table.
    void setHostModes(std::vector<HostMode> modes);
    // The table with the host's figures, cut to what it offers: what the
    // map draws.
    const SubmodeTable& usable() const { return usable_; }

    void setWidth(Width width);
    Width width() const { return width_; }
    void setPath(Path path);
    Path path() const { return path_; }

    // With Auto on and a path S/N known, the gear in effect is Auto's pick;
    // with no S/N reports yet, it stays the chosen gear.
    void setAuto(bool on) { auto_ = on; }
    bool isAuto() const { return auto_; }

    // Chooses a gear by number (from 1) and turns Auto off. False if there
    // is no such gear.
    bool selectGear(int number);

    void recordBurstSnr(double dB) { snr_.record(dB); }
    void clearSnr() { snr_.clear(); }
    std::optional<double> pathSnr() const { return snr_.average(); }
    std::size_t snrBursts() const { return snr_.count(); }

    // The ladder for the current width, path and offered modes.
    const std::vector<Gear>& ladder() const { return ladder_; }
    // The chosen gear (Auto off), even while Auto overrides it.
    std::optional<Gear> chosen() const;
    // Auto's pick, whether or not Auto is on. Empty with no path S/N.
    std::optional<Gear> suggestion() const;
    // The gear in effect: Auto's pick or the chosen gear.
    std::optional<Gear> current() const;

private:
    void rebuild();

    SubmodeTable table_;
    SubmodeTable usable_;
    std::vector<HostMode> hostModes_;
    Width width_ = Width::Hz2400;
    Path path_ = Path::Calm;
    bool auto_ = false;
    PathSnr snr_;
    std::vector<Gear> ladder_;
    int chosen_ = 0;  // index into ladder_
};

} // namespace ragtime::gearbox

#endif
