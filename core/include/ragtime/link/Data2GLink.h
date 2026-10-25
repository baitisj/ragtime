// The byte formats Ragtime uses to talk to a separately running
// data2g-host: KISS framing (with ACKMODE), the command port's lines, and
// the modes it offers. No sockets here, so all of it is tested on its own.
//
// Ported from Glissando's Data2GLink (written there for Glissando from the
// public KISS and VARA protocol descriptions and Data2G's
// docs/broadcast.md) and relicensed MIT by Jeff. No Data2G code is used.
// The session stream's framing is Ragtime's own: see
// ragtime/protocol/Stream.h.
#ifndef RAGTIME_LINK_DATA2G_LINK_H
#define RAGTIME_LINK_DATA2G_LINK_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ragtime
{
namespace link
{

constexpr int DEFAULT_KISS_PORT = 8100;
constexpr int DEFAULT_COMMAND_PORT = 8300; // the session data port is the next one

// The broadcast group Ragtime opens. Its bursts carry the name, and every
// codeword a CRC mask hashed from it, so only stations that opened D2G
// take them in. Never GLISS: Ragtime and Glissando don't read each other.
constexpr const char* GROUP = "D2G";

//-------------------------------------------------------------------------
// KISS
//-------------------------------------------------------------------------

constexpr uint8_t KISS_DATA = 0x00;
constexpr uint8_t KISS_ACKMODE = 0x0C;
constexpr uint8_t KISS_PERSIST = 0x02;      // KISS P: one byte, a slot taken with probability (P + 1) / 256

// A data frame for a KISS port (0 to 15).
std::vector<uint8_t> kissEncode(int port, const std::vector<uint8_t>& data);

// An ACKMODE frame: data2g-host answers with the same tag once the burst
// carrying it has been transmitted.
std::vector<uint8_t> kissEncodeAckMode(int port, uint16_t tag, const std::vector<uint8_t>& data);
// A KISS command with a one-byte value, such as KISS_PERSIST.
std::vector<uint8_t> kissEncodeCommand(int port, uint8_t command, uint8_t value);

struct KissFrame
{
    int port = 0;
    uint8_t command = KISS_DATA;    // low nibble of the command byte
    std::vector<uint8_t> payload;   // data, or for an ACKMODE ack the 2-byte tag
};

// Splits a KISS byte stream into frames, however it arrives in pieces.
class KissDecoder
{
public:
    void feed(const uint8_t* bytes, size_t length, std::vector<KissFrame>& framesOut);
    void reset();

private:
    std::vector<uint8_t> current_;
    bool escaped_ = false;
    bool inFrame_ = false;
};

//-------------------------------------------------------------------------
// Modes
//-------------------------------------------------------------------------

// One line of the MODES list: "MODE name bandwidth-Hz bytes-per-codeword
// max-codewords seconds-at-1 seconds-at-max".
struct ModeInfo
{
    std::string name;
    int bandwidthHz = 0;
    int bytesPerCodeword = 0;
    int maxCodewords = 0;
    double secondsAtOne = 0.0;
    double secondsAtMax = 0.0;

    bool valid() const { return !name.empty() && bytesPerCodeword > 0 && maxCodewords > 0; }

    // How long a burst carrying these frames takes: one control codeword,
    // then the frames back to back behind a 2-byte length each.
    double burstSeconds(const std::vector<int>& frameBytes) const;
};

// The modes a 2400 Hz data2g-host offered when this was written (Data2G
// 8ffe15c, as Glissando measured them); what the host's own MODES list
// says wins.
const std::vector<ModeInfo>& knownModes();

// The operator's 500 Hz setting: of the modes offered (or, none listed
// yet, the known ones), those no wider than this.
constexpr int NARROW_MODE_MAX_HZ = 500;
std::vector<ModeInfo> narrowModes(const std::vector<ModeInfo>& offered);

//-------------------------------------------------------------------------
// The command port
//-------------------------------------------------------------------------

// What a line from the command port means to us.
struct CommandEvent
{
    enum class Type
    {
        Other,          // IAMALIVE, VERSION and anything Ragtime has no use for
        Ok,
        Wrong,
        Ptt,            // on
        Busy,           // on
        Mode,           // text: the submode a burst went out in
        ModeLine,       // modeInfo: one line of the MODES list
        BcastPort,      // number: the port BCAST OPEN gave
        BcastHeard,     // number, text: the sender's call when sent with FROM, snr
        BcastLost,      // number, count
        BcastDropped,   // number, count
        BcastMissed,    // text: the submode, count: its codewords, snr (any port's: "BCAST *")
        Sn,             // snr: the session partner's last burst (the answer to SN)
        Connected,      // text: caller, peer: called station, number: bandwidth when given
        Disconnected,
        Buffer,         // count: bytes still to send in the session
        Cqframe,
        Clean,          // clean: a CLEANTXBUFFER answer, or its SETTLED report
    } type = Type::Other;

    bool on = false;
    int number = 0;
    int count = 0;
    std::string text;
    std::string peer;
    ModeInfo modeInfo;
    // dB, average power in 2500 Hz, on the gearshift's scale. Hosts since
    // Data2G PR #98 end HEARD with the burst's pilot SNR and MISSED with an
    // energy reading (once they know the noise floor); older hosts send
    // neither, and a HEARD without one still parses.
    std::optional<double> snr;

    // CLEANTXBUFFER (Data2G PR #118; with DATA2G ON, host byte offsets
    // since CONNECTED, ranges [from, to)):
    // - Ok: drop from, drop to, limbo from, limbo to: the bytes dropped,
    //   and those on air not yet acknowledged.
    // - Late: nothing dropped, the offset asked for is on air already.
    // - Empty (BUFFEREMPTY): nothing queued. Failed: no session.
    // - Settled: from, split, to: the limbo settled, delivered before split,
    //   dropped from it; unknown when the session ended first.
    struct Clean
    {
        enum class Result
        {
            Ok,
            Late,
            Empty,
            Failed,
            Settled,
        } result = Result::Ok;
        std::vector<uint64_t> offsets;
        bool unknown = false;
    } clean;
};

CommandEvent parseCommandLine(const std::string& line);

// Splits the command port's CR (or LF) terminated lines.
class LineSplitter
{
public:
    void feed(const char* bytes, size_t length, std::vector<std::string>& linesOut);
    void reset() { current_.clear(); }

private:
    std::string current_;
};

// The callsign as the command port and a BCAST FROM can carry it: letters,
// digits, '/' and '-', at most 10 characters, upper case.
std::string commandCallsign(const std::string& callsign);

} // namespace link
} // namespace ragtime

#endif
