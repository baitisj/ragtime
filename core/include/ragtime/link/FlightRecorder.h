// FlightRecorder: everything that crosses the link to data2g-host, kept on
// disk with millisecond times, so a tester on a fading path sends a file
// instead of a recollection (Help > Save a report), and a builder plays
// the host's side back with ragtime_fake_air --replay
// (docs/FLIGHT_RECORDER.md).
//
// One line a thing, "<UTC time> <tag> <what>", ">" being to the host and
// "<" from it:
//
//   2026-10-25T16:25:42.123Z up 127.0.0.1 KISS 8100 commands 8300
//   2026-10-25T16:25:42.125Z cmd> MYCALL AG7EW
//   2026-10-25T16:25:42.131Z cmd< OK
//   2026-10-25T16:25:51.402Z kiss< 0000a0...      (a frame between FENDs, hex)
//   2026-10-25T16:26:03.010Z data-up
//   2026-10-25T16:26:03.500Z data> 312            (session bytes: counts only)
//   2026-10-25T16:31:12.000Z down data2g-host closed its command port
//   2026-10-25T16:31:12.000Z note Ragtime 0.1.2 opened
//
// The files roll over at each UTC day and at 5 MB, and are kept 7 days
// and 50 MB at most, the oldest going first. The time is always passed in,
// so tests run it on a simulated clock. No sockets here: HostConnection
// is the one place that taps it.
#ifndef RAGTIME_LINK_FLIGHT_RECORDER_H
#define RAGTIME_LINK_FLIGHT_RECORDER_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ragtime
{
namespace link
{

class FlightRecorder
{
public:
    static constexpr uint64_t KEEP_MS = 7ull * 24 * 60 * 60 * 1000;
    static constexpr uint64_t CAP_BYTES = 50ull * 1024 * 1024;
    static constexpr uint64_t FILE_BYTES = 5ull * 1024 * 1024;
    // A line or frame longer than this is written in pieces.
    static constexpr size_t MAX_PIECE = 4096;

    FlightRecorder() = default;
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // Where the files go ("" stops recording). The folder is made when
    // the first line is written. Writes "note Ragtime <version> opened".
    void setFolder(const std::string& folder, uint64_t nowMs);
    const std::string& folder() const { return folder_; }
    // For tests: smaller files and cap.
    void setLimits(uint64_t fileBytes, uint64_t capBytes);

    // The KISS and command ports are both up, or one has closed.
    void up(const std::string& address, uint64_t nowMs);
    void down(const std::string& why, uint64_t nowMs);
    void commandIn(const char* bytes, size_t length, uint64_t nowMs);
    void commandOut(const char* bytes, size_t length, uint64_t nowMs);
    void kissIn(const uint8_t* bytes, size_t length, uint64_t nowMs);
    void kissOut(const uint8_t* bytes, size_t length, uint64_t nowMs);
    // The session data port: up and down, and byte counts each way.
    void dataUp(bool up, uint64_t nowMs);
    void dataIn(size_t length, uint64_t nowMs);
    void dataOut(size_t length, uint64_t nowMs);
    // Anything else worth a line.
    void note(const std::string& text, uint64_t nowMs);

    // The recorder's files, oldest first: names in folder().
    std::vector<std::string> fileNames() const;
    // Drops files past the week and past the cap, never the one open.
    void prune(uint64_t nowMs);

private:
    // Bytes on the way in or out, cut into lines (command port) or frames
    // (KISS) before they are written.
    struct Pieces
    {
        std::string pending;
        bool started = false;   // KISS: a FEND has been seen
    };

    void write(const std::string& tag, const std::string& what, uint64_t nowMs);
    void roll(uint64_t nowMs);
    void lines(Pieces& pieces, const char* tag, const char* bytes, size_t length, uint64_t nowMs);
    void frames(Pieces& pieces, const char* tag, const uint8_t* bytes, size_t length, uint64_t nowMs);
    void flushPieces(uint64_t nowMs);

    std::string folder_;
    uint64_t fileLimit_ = FILE_BYTES;
    uint64_t capBytes_ = CAP_BYTES;
    std::ofstream out_;
    std::string fileName_;
    int64_t fileDay_ = -1;      // days since 1970 of the open file
    uint64_t fileBytes_ = 0;
    uint64_t failedMs_ = 0;     // opening failed then; tried again a minute on
    bool failed_ = false;
    Pieces cmdIn_, cmdOut_, kissIn_, kissOut_;
};

// One recorded line, read back.
struct FlightLine
{
    enum class Kind
    {
        Up,         // text: the address
        Down,       // text: why
        CommandIn,  // text: the line from the host
        CommandOut, // text: our line
        KissIn,     // bytes: the frame from the host, as on the wire between FENDs
        KissOut,
        DataUp,
        DataDown,
        DataIn,     // count
        DataOut,
        Note,       // text
    } kind = Kind::Note;
    uint64_t ms = 0;
    std::string text;
    std::vector<uint8_t> bytes;
    size_t count = 0;
};

// One line of a recording; false for one that isn't (a blank, a damaged
// or a newer kind of line).
bool parseFlightLine(const std::string& line, FlightLine& out);
// A whole recording's lines, in the order written.
std::vector<FlightLine> parseFlight(const std::string& text);

// "2026-10-25T16:25:42.123Z".
std::string flightTime(uint64_t ms);
// Reads that back, and shorter forms of it: "2026-10-25T16:25",
// "2026-10-25 16:25:42" (Z optional).
bool parseFlightTime(const std::string& text, uint64_t& ms);

// A command line as written: printable ASCII as it is, a backslash
// doubled, anything else \xNN. And back.
std::string escapeCommandLine(const std::string& line);
std::string unescapeCommandLine(const std::string& text);

// Whether a setting may go in a report: never a private key, a password,
// recovery words or anything else named like a secret, whatever later
// packages call theirs. The path is the setting's full name,
// "/Station/Callsign".
bool reportableSetting(const std::string& path);

} // namespace link
} // namespace ragtime

#endif
