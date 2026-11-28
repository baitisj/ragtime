// RigFilter: the rig's receive filter as it reports it, for the Ear
//: its mode, the passband width
// Hamlib gives with the mode, and the twin passband tuning (PBT_IN and
// PBT_OUT), read between bursts while the Ear is open, through rigctld
// ("+m", "+l PBT_IN", "+l PBT_OUT") or the rig opened with Hamlib. Read
// only: nothing here sets the rig.
//
// Hamlib gives PBT as the rig gives it. Most Icoms, the IC-7100 among
// them, give the knob's position, 0 to 1 with 0.5 in the middle (the
// rig's 0 to 255, over 255), not Hz; a few rigs give Hz either side of 0.
// A value from -1 to 1 is taken for a knob, anything bigger for Hz.
//
// A rig with no PBT may have IF SHIFT instead (a Yaesu: Hamlib's IF
// level, Hz either side of 0, +-1200 on an FT-891 in 20 Hz steps), which
// moves the whole passband; read only from a rig with no PBT.
//
// The manual notch, where the rig has one (Hamlib's MN function): on or
// off, and where. An Icom gives where as its NOTCHF_RAW level, 0 to 1
// (the rig's 0 to 255): like PBT, a position, not Hz, which
// ear::NotchModel turns into Hz. Yaesu and Kenwood rigs (an FT-891, say)
// have no NOTCHF_RAW and give their NOTCHF level instead, the notch's
// centre in audio Hz (in 10 Hz steps on a Yaesu).
//
// Some rigs can change the notch's width too, in a few steps (Narrow and
// Wide, or Wide, Mid and Narrow). Hamlib has no standard level for it, so
// it is found by name among the rig's extra levels and parameters
// (notchWidthName): today only an FTX-1's DSP_IF_NOTCH_W, a parameter, 0
// or 1. Through rigctld that one name is asked ("+p DSP_IF_NOTCH_W"),
// as rigctld can't say which extra ones a rig has. The width is the step
// as the rig numbers it, 0 up; what each step measures is the Ear's to
// find (ear::NotchModel).
#ifndef RAGTIME_RADIO_RIG_FILTER_H
#define RAGTIME_RADIO_RIG_FILTER_H

#include <optional>
#include <string>

namespace ragtime
{
namespace radio
{

struct RigFilter
{
    std::string mode;               // Hamlib's name: "USB", "PKTUSB"
    int widthHz = 0;                // the passband with the mode; 0 if none
    std::optional<double> pbtIn;    // as the rig gives it
    std::optional<double> pbtOut;
    std::optional<int> ifShiftHz;   // IF SHIFT, Hz, where there is no PBT
    std::optional<bool> notchOn;    // the manual notch
    std::optional<double> notchRaw; // its position, 0 to 1
    std::optional<int> notchHz;     // or its centre in Hz (NOTCHF)
    std::optional<int> notchWidth;  // its width: a step, 0 up, of
    int notchWidths = 0;            // this many; 0 where the rig has none

    bool operator==(const RigFilter& o) const
    {
        return mode == o.mode && widthHz == o.widthHz && pbtIn == o.pbtIn && pbtOut == o.pbtOut &&
               ifShiftHz == o.ifShiftHz &&
               notchOn == o.notchOn && notchRaw == o.notchRaw && notchHz == o.notchHz &&
               notchWidth == o.notchWidth && notchWidths == o.notchWidths;
    }
    bool operator!=(const RigFilter& o) const { return !(*this == o); }
};

// A PBT reading in the middle: a knob within a step of 0.5, or under 1 Hz.
bool pbtCentred(double value);
// "0.62" for a knob, "+120 Hz" for Hz.
std::string pbtText(double value);
// "USB · 2400 Hz filter · PBT centred", "PKTUSB · 3000 Hz filter · PBT
// in 0.62, out 0.50 · notch 0.68", "USB · 2400 Hz filter · IF shift
// +200 Hz"; parts the rig didn't give are left out, and the notch while it
// is off.
std::string describe(const RigFilter& filter);
// "IF +200 Hz" for the flight recorder; empty from a rig without it.
std::string ifShiftText(const RigFilter& filter);
// The notch for the flight recorder: "NF off", "NF 0.68", "NF 1500 Hz",
// or "NF on" from a rig that says nothing of where, then its width step
// where the rig has them ("NF 1500 Hz W1/2"); empty from a rig with no
// notch.
std::string notchText(const RigFilter& filter);

// The notch's width through rigctld: the one extra parameter asked, and
// its steps (an FTX-1's, 0 or 1).
constexpr const char* NOTCH_WIDTH_PARM = "DSP_IF_NOTCH_W";
constexpr int NOTCH_WIDTH_PARM_STEPS = 2;
// Whether a Hamlib extra level's or parameter's name is a notch width:
// "NOTCH" in it with "WIDTH" or ending "_W" ("DSP_IF_NOTCH_W"), and not
// the audio peak filter's or the contour's.
bool notchWidthName(const std::string& name);

} // namespace radio
} // namespace ragtime

#endif
