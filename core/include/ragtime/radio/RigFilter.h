// RigFilter: the rig's receive filter as it reports it, for the Ear
// (docs/STAGE1.md, "The Ear", step 2): its mode, the passband width
// Hamlib gives with the mode, and the twin passband tuning (PBT_IN and
// PBT_OUT), read between bursts while the Ear is open, through rigctld
// ("+m", "+l PBT_IN", "+l PBT_OUT") or the rig opened with Hamlib. Read
// only: nothing here sets the rig.
//
// Hamlib gives PBT as the rig gives it. Most Icoms, the IC-7100 among
// them, give the knob's position, 0 to 1 with 0.5 in the middle (the
// rig's 0 to 255, over 255), not Hz; a few rigs give Hz either side of 0.
// A value from -1 to 1 is taken for a knob, anything bigger for Hz.
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

    bool operator==(const RigFilter& o) const
    {
        return mode == o.mode && widthHz == o.widthHz && pbtIn == o.pbtIn && pbtOut == o.pbtOut;
    }
    bool operator!=(const RigFilter& o) const { return !(*this == o); }
};

// A PBT reading in the middle: a knob within a step of 0.5, or under 1 Hz.
bool pbtCentred(double value);
// "0.62" for a knob, "+120 Hz" for Hz.
std::string pbtText(double value);
// "USB · 2400 Hz filter · PBT centred", "PKTUSB · 3000 Hz filter · PBT
// in 0.62, out 0.50"; parts the rig didn't give are left out.
std::string describe(const RigFilter& filter);

} // namespace radio
} // namespace ragtime

#endif
