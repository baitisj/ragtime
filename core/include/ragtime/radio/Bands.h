// The amateur bands, and where US rules let a station send data
// (docs/PLAN.md, "Frequency plans"). The radio's dial frequency comes
// from rigctld (radio::DialReader); this names its band for the path log,
// and says whether a Data2G burst may go out there.
//
// Ragtime never tunes the radio. The data segment check is the operator's
// choice ("Send only inside the US data segments", off by default); it
// knows nothing of licence classes, so staying inside one's own
// privileges is still the operator's business.
#ifndef RAGTIME_RADIO_BANDS_H
#define RAGTIME_RADIO_BANDS_H

#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace radio
{

// One stretch of spectrum, lowHz to highHz inclusive, and its band's name.
struct Span
{
    uint64_t lowHz = 0;
    uint64_t highHz = 0;
    const char* band = "";
};

// The US amateur bands, edge to edge (47 CFR 97.301), lowest first:
// "160 m" to "23 cm".
const std::vector<Span>& amateurBands();

// The band a dial frequency is in: "20 m", or "" outside the bands.
std::string bandOf(uint64_t hz);
// A band's place in amateurBands(), for sorting; past the end for "".
size_t bandOrder(const std::string& band);

// The US amateur segments where data is allowed (47 CFR 97.305(c), with
// 97.301's edges where a band allows data throughout), lowest first,
// with 60 m's band segment (below). Left out on purpose: 2200 m and
// 630 m (notice to the utilities council first, 97.303(g)), 60 m's
// channels (below: a place on the dial, not a stretch of it), and
// 219-220 MHz (point-to-point message forwarding only).
const std::vector<Span>& usDataSegments();

// 60 m in the US (47 CFR 97.303(h), as FCC 25-60 left it from 13 February
// 2026): four channels, centred on 5332, 5348, 5373 and 5405 kHz, where
// phone, data and RTTY have the carrier 1.5 kHz below the centre, at up
// to 100 W ERP; and the band segment 5351.5-5366.5 kHz, at up to 9.15 W
// ERP (15 W EIRP; 97.313(i)). Nothing wider than 2.8 kHz, General class
// and up. The old 5358.5 kHz channel is inside the segment now.
const std::vector<uint64_t>& us60mChannelCentres();
const Span& us60mSegment();
// Data2G centres every mode on 1500 Hz of audio, so in USB a burst is
// centred on a 60 m channel only with the dial exactly this far below the
// channel's centre: 5371.5 kHz for 5373 kHz (Jeff, 2026-10-27: "tx only
// on center minus 1.5 kHz", to the hertz).
constexpr uint64_t US_60M_DIAL_BELOW_HZ = 1500;

// The dial is what rigctld reports, and the burst sits up to about
// 2.8 kHz to one side of it depending on the sideband, so the dial must
// be this far inside a segment for the whole burst to be.
constexpr uint64_t SEGMENT_MARGIN_HZ = 3000;

// Empty when a Data2G burst may go out with the dial here; else why not,
// for the operator, naming the nearest segment: "14.200 MHz is outside
// the US data segments (47 CFR 97.305). The nearest is 14.000-14.150 MHz
// (20 m), with 3 kHz to spare at each edge." On 60 m it names the
// channels' dial settings and the band segment instead.
std::string usDataRefusal(uint64_t dialHz);

// Where the dial lets a burst go out, for the dial's tooltip: "Inside a
// US data segment: sending allowed", with 60 m's power limit there. Empty
// where usDataRefusal() refuses.
std::string usDataPlace(uint64_t dialHz);

// "14.105 MHz", "7.0745 MHz": MHz with at least three decimals and at
// most six, trailing zeros cut.
std::string formatMHz(uint64_t hz);

} // namespace radio
} // namespace ragtime

#endif
