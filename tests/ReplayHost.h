// A stand-in for data2g-host that plays a flight recording's host side
// back (link::FlightRecorder, docs/FLIGHT_RECORDER.md), for
// ragtime_fake_air --replay: a builder sees the exact lines and frames, at
// the exact spacing, that a tester's Ragtime saw on a fading path.
//
// The recording is cut into its connections, each from an "up" line to a
// "down". When a Ragtime connects to the command port, the next connection
// plays: each "cmd<" line goes to the command port and each "kiss<" frame
// to the KISS port, as long after the Ragtime's first command line as it
// came after the recorded one (divided by the speed), so the host's
// answers never come before the questions. At its "down" the ports close, and the
// next connection plays when the Ragtime connects again. What the Ragtime
// writes is read and logged, never answered: the recording answers.
// Session bytes were only counted, so the data port takes a connection
// and logs the recorded counts but carries nothing.
//
// Everything runs on the caller's thread: step() takes what has arrived
// and plays what is due.
#ifndef RAGTIME_TESTS_REPLAY_HOST_H
#define RAGTIME_TESTS_REPLAY_HOST_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ragtime/link/FlightRecorder.h"

namespace ragtime_test
{

class ReplayHost
{
public:
    struct Options
    {
        double speed = 1.0;     // 2: twice as fast
        uint64_t fromMs = 0;    // start at this recorded time (0: the start)
    };

    // On fixed ports, for the bench (0: any free one), the data port being
    // the command port + 1. Throws if they're taken.
    ReplayHost(const std::vector<ragtime::link::FlightLine>& recording, Options options, int kissPort = 0,
               int commandPort = 0);
    ~ReplayHost();

    ReplayHost(const ReplayHost&) = delete;
    ReplayHost& operator=(const ReplayHost&) = delete;

    int kissPort() const { return kissPort_; }
    int commandPort() const { return commandPort_; }
    // The recording's connections, and how many have played to the end.
    size_t connections() const { return segments_.size(); }
    size_t played() const { return played_; }
    bool finished() const { return played_ == segments_.size(); }

    // Waits up to timeoutMs for something to arrive, and plays what is due.
    void step(int timeoutMs = 20);

    // What happens, a line a thing, starting with the recorded time.
    std::function<void(const std::string&)> onLog;
    // The lines the Ragtime wrote to the command port.
    const std::vector<std::string>& heard() const { return heard_; }

private:
    struct Segment
    {
        uint64_t startMs = 0;
        uint64_t endMs = 0;
        bool closes = false;    // it ends with "down"
        uint64_t firstOutMs = 0;    // the Ragtime's first command line (0: none)
        std::vector<ragtime::link::FlightLine> lines;  // the host's side
    };

    void log(uint64_t recordedMs, const std::string& what);
    void accept();
    void readClients();
    void play();
    void startClock(uint64_t recordedMs);
    void closeClients();
    uint64_t recordedNow() const;
    void send(int fd, const std::string& bytes);

    std::vector<Segment> segments_;
    double speed_ = 1.0;
    int kissListener_ = -1, commandListener_ = -1, dataListener_ = -1;
    int kiss_ = -1, command_ = -1, data_ = -1;
    int kissPort_ = 0, commandPort_ = 0;
    std::string commandIn_;
    std::vector<uint8_t> kissIn_;

    size_t current_ = 0;        // the connection playing or next
    size_t next_ = 0;           // its next line
    bool playing_ = false;
    long long startedMs_ = 0;   // our clock when it began playing
    long long closeAtMs_ = -1;  // the ports close then, the connection played
    long long waitingSinceMs_ = -1; // connected, its clock waiting for the first command line
    size_t played_ = 0;
    std::vector<std::string> heard_;
};

// A recording read from files or folders (a folder: every flight-*.log in
// it), in time order; false, with why, if one can't be read.
bool loadRecording(const std::vector<std::string>& paths, std::vector<ragtime::link::FlightLine>& out,
                   std::string& why);

} // namespace ragtime_test

#endif
