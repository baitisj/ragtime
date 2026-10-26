// RigSettings: how Ragtime reaches the radio, from Settings > Rig.
//
// Three ways, picked at the top of the page:
//
//   - None: Ragtime doesn't know the band.
//   - rigctld: Ragtime reads the dial from a rigctld already running
//     (DialReader), asking "\chk_vfo", "f" and the meters ("+l SWR")
//     and nothing else, since the operator's rigctld is often shared
//     (fldigi). Never keys.
//   - Hamlib: Ragtime opens the rig itself with Hamlib, as other ham
//     programs do: any rig Hamlib knows, on a serial port or a network
//     address, with the usual serial settings and the backend's own
//     (civaddr=0x88). It reads the dial, and, only when keyRig is ticked
//     (off by default), keys the rig when data2g-host reports PTT ON and
//     unkeys at PTT OFF (PttFollower). Andrew's suggestion (2026-10-26),
//     for rigs whose drivers can't be opened by two programs: run
//     data2g-host with --no-rig and let Ragtime hold the rig.
//
// hamlibConf() turns the settings into Hamlib's own configuration words
// (the names `rigctl -m <model> -L` lists), which the rig library hands to
// rig_set_conf() before opening. Nothing here talks to Hamlib.
#ifndef RAGTIME_RADIO_RIG_SETTINGS_H
#define RAGTIME_RADIO_RIG_SETTINGS_H

#include <string>
#include <utility>
#include <vector>

namespace ragtime
{
namespace radio
{

enum class RigSource
{
    None,
    Rigctld,
    Hamlib,
};

// How Ragtime keys the rig when keyRig is on.
enum class PttMethod
{
    CatData,    // a CAT command, audio from the rear data port
    CatMic,     // a CAT command, the radio's usual audio
    Dtr,        // the DTR line of a serial port
    Rts,        // the RTS line
};

// Serial settings left at Default take the rig backend's own.
enum class Parity
{
    Default,
    None,
    Odd,
    Even,
    Mark,
    Space,
};

enum class Handshake
{
    Default,
    None,
    XonXoff,
    Hardware,
};

// DTR and RTS held on or off (some interfaces draw power from them).
enum class LineState
{
    Default,
    On,
    Off,
};

struct RigSettings
{
    RigSource source = RigSource::None;

    // rigctld, read only.
    std::string rigctldHost = "127.0.0.1";
    int rigctldPort = 4532;

    // Hamlib.
    int model = 0;                  // Hamlib's rig model number; 0: none picked
    std::string port;               // /dev/ttyUSB0, COM3, or host:port for a network rig
    int baud = 0;                   // 0, and the data and stop bits: the rig's own
    int dataBits = 0;
    int stopBits = 0;
    Parity parity = Parity::Default;
    Handshake handshake = Handshake::Default;
    LineState dtr = LineState::Default;
    LineState rts = LineState::Default;
    std::string backendConf;        // "civaddr=0x88 timeout=500"
    int pollSeconds = 1;            // the dial is read this often

    // Keying, Hamlib only, off by default.
    bool keyRig = false;
    PttMethod ptt = PttMethod::CatData;
    std::string pttPort;            // DTR or RTS on another port; empty: the rig's port

    // Over 3:1 SWR, stop data2g-host, and with Hamlib turn the rig's power
    // to 0 and unkey (radio::SwrTrip). Off by default.
    bool swrTrip = false;
};

constexpr int POLL_MIN_SECONDS = 1;
constexpr int POLL_MAX_SECONDS = 30;

// The words kept in the settings file, and back. An unknown word reads as
// the first value.
const char* sourceWord(RigSource source);
RigSource sourceFromWord(const std::string& word);
const char* pttWord(PttMethod ptt);
PttMethod pttFromWord(const std::string& word);
// Parity, handshake and line states are kept as Hamlib's own words
// ("Even", "XONXOFF", "ON"); Default is "".
const char* parityWord(Parity parity);
Parity parityFromWord(const std::string& word);
const char* handshakeWord(Handshake handshake);
Handshake handshakeFromWord(const std::string& word);
const char* lineWord(LineState state);
LineState lineFromWord(const std::string& word);

// What the Rig page calls the PTT methods.
const char* pttLabel(PttMethod ptt);

// Splits "civaddr=0x88 timeout=500" (spaces, commas or semicolons between)
// into (name, value) pairs. False, with a reason in problem, for a word
// without "=" or with an empty name.
bool parseBackendConf(const std::string& text, std::vector<std::pair<std::string, std::string>>& out,
                      std::string& problem);

// What a Hamlib rig model is reached by.
enum class PortType
{
    None,       // nothing (Hamlib's dummy rig)
    Serial,     // a serial port, with baud, bits, parity and so on
    Network,    // host:port
    Other,      // USB and the like: a device name, no serial settings
};

// Hamlib's configuration for these settings, in the order to set it: the
// port, the serial settings when the rig is on a serial port (a network
// rig refuses them), the PTT type when keying (and the port, by DTR or
// RTS), then the backend's own words. Empty, with a reason in problem, if backendConf
// doesn't parse or a rig that needs a port has none.
std::vector<std::pair<std::string, std::string>> hamlibConf(const RigSettings& settings, PortType port,
                                                            std::string& problem);

// The same rig, opened the same way: the poll interval and the SWR trip
// aside, which apply to an open rig, and the PTT method and port while
// not keying.
bool sameRig(const RigSettings& a, const RigSettings& b);

} // namespace radio
} // namespace ragtime

#endif
