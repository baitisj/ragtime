// A stand-in for data2g-host, for tests: the KISS, command and session
// data ports on 127.0.0.1, with replies scripted by the test. It is written
// for Ragtime from the command lines and reports in docs/PROTOCOL.md and
// Glissando's docs/DATA2G.md; no Data2G code is used, and a real
// data2g-host never runs in a test.
//
// Several hosts share one FakeAir: a burst one host sends reaches the
// group of every other host there, and a session joins two hosts' data
// ports. Everything runs on the test's own thread: FakeAir::step() takes
// what has arrived on every port and answers it.
//
// What a host does, unless the test says otherwise:
// - MYCALL, CHAT, LISTEN, BW500, BW2300, BCAST MODE: OK.
// - BCAST OPEN <group> FROM <call>: BCAST PORT <groupPort>.
// - MODES: a MODE line for each of link::knownModes(), then OK.
// - CONNECT <me> <them>: OK, then CONNECTED on both hosts if a host on
//   the air is <them> and listening; DISCONNECTED otherwise.
// - DISCONNECT and ABORT: OK, then DISCONNECTED on both.
// - ACKMODE frames on the group port: sent as one burst at the next step
//   unless a session is up (it holds the group) or bursts are held. A burst
//   goes out as PTT ON, MODE <mode>, BCAST <port> HEARD <call> and the
//   frames on every other host on the air, the ACKMODE acks, PTT OFF.
// - Session bytes: BUFFER <n> once read, carried to the far end, BUFFER 0.
#ifndef RAGTIME_TESTS_FAKE_HOST_H
#define RAGTIME_TESTS_FAKE_HOST_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <poll.h>

#include "ragtime/link/Data2GLink.h"

namespace ragtime_test
{

class FakeAir;

class FakeHost
{
public:
    explicit FakeHost(FakeAir& air, int groupPort = 2);
    ~FakeHost();

    FakeHost(const FakeHost&) = delete;
    FakeHost& operator=(const FakeHost&) = delete;

    int kissPort() const { return kissPort_; }
    int commandPort() const { return commandPort_; }
    int dataPort() const { return commandPort_ + 1; }

    // Scripted replies: a command starting with prefix is answered with
    // these lines (each sent with CR) in place of the usual reply.
    void script(const std::string& prefix, const std::vector<std::string>& replies);
    // A report out of the blue, sent at once: "BUSY ON" and so on.
    void say(const std::string& line);
    // Bursts written to the group wait here until let go.
    void holdBursts(bool hold) { holdBursts_ = hold; }
    // The next burst is dropped: BCAST <port> DROPPED <frames>, no acks.
    void dropNextBurst() { dropNext_ = true; }
    // Closes the command port's client, as a host that stopped would.
    void closeCommand();

    // What the host has been told.
    const std::vector<std::string>& commands() const { return commands_; }
    const std::string& callsign() const { return call_; }
    bool listening() const { return listening_; }
    bool groupOpen() const { return groupOpen_; }
    bool inSession() const { return partner_ != nullptr; }
    size_t burstsSent() const { return burstsSent_; }

private:
    friend class FakeAir;

    struct Port
    {
        int listener = -1;
        int client = -1;
        std::vector<uint8_t> in;
    };

    void pollFds(std::vector<pollfd>& fds);
    void serviceCommand();
    void serviceKiss();
    void serviceData();
    void onCommand(const std::string& line);
    void reply(const std::string& line);
    void sendBurst();
    void connected(FakeHost& caller, FakeHost& called);
    void disconnected();
    void writeTo(Port& port, const std::vector<uint8_t>& bytes);

    FakeAir& air_;
    int groupPort_;
    int kissPort_ = 0;
    int commandPort_ = 0;
    Port kiss_, command_, data_;
    std::string lineIn_;
    ragtime::link::KissDecoder kissDecoder_;

    std::map<std::string, std::vector<std::string>> scripts_;
    std::vector<std::string> commands_;
    std::string call_;
    std::string from_;          // the call BCAST OPEN gave
    std::string mode_ = "qpsk-r1/3";
    bool listening_ = false;
    bool groupOpen_ = false;
    bool holdBursts_ = false;
    bool dropNext_ = false;
    struct Tagged
    {
        uint16_t tag;
        std::vector<uint8_t> frame;
    };
    std::vector<Tagged> burst_;
    size_t burstsSent_ = 0;
    FakeHost* partner_ = nullptr;
};

class FakeAir
{
public:
    // Takes what has arrived on every host's ports, waiting up to
    // timeoutMs for something, and answers it all.
    void step(int timeoutMs = 2);

private:
    friend class FakeHost;
    std::vector<FakeHost*> hosts_;
};

} // namespace ragtime_test

#endif
