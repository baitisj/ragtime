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
// - CONNECT <me> <them>: OK, then up to five calls, each keying up (PTT
//   ON, PTT OFF after the airtime). CONNECTED on both hosts at the call
//   the far host answers on, if a host on the air is <them>, listening and
//   free; DISCONNECTED after the fifth otherwise.
// - DISCONNECT: OK, then DISCONNECTED on both once nothing is waiting to
//   cross. ABORT: OK and DISCONNECTED at once, on both.
// - ACKMODE frames on the group port: sent as one burst at the next step
//   unless a session is up (it holds the group) or bursts are held. A burst
//   goes out as PTT ON, MODE <mode>, BCAST <port> HEARD <call> and the
//   frames on every other host on the air, the ACKMODE acks, PTT OFF.
// - Session bytes: BUFFER <n> once read (n: all still waiting to cross),
//   carried to the far end after the airtime, then BUFFER with what is
//   left. A fading session holds them, read and counted but not carried.
// - With setIdleBroadcastMs, bursts go while a session has been idle that
//   long, as data2g-host does since Data2G PR #59; otherwise a session
//   holds the group for as long as it lasts.
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
    // On fixed ports, for the bench (0: any free one). Throws if they're taken.
    FakeHost(FakeAir& air, int groupPort, int kissPort, int commandPort);
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
    // For the bench: a burst keeps PTT on this long before the far hosts
    // hear it and the acks come back (0, the tests' default: all at once).
    void setAirtimeMs(int ms) { airtimeMs_ = ms; }
    // Calls: the host called answers on this call, 1 to 5 (default 1).
    void setAnswerOnCall(int call) { answerOn_ = call; }
    // A deep fade: session bytes are read and counted, but neither carried
    // nor acknowledged until it lifts.
    void fadeSession(bool fade) { fading_ = fade; }
    // Session bytes cross at most this fast, bytes a second, after the
    // airtime (0, the default: all at once).
    void setSessionRate(int bytesPerSecond) { sessionRate_ = bytesPerSecond; }
    // Bursts go while a session has been idle this long (0: never).
    void setIdleBroadcastMs(int ms) { idleBroadcastMs_ = ms; }
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
    bool calling() const { return calling_; }
    // Session bytes read from our client, not yet carried.
    size_t sessionWaiting() const { return waiting_; }
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
    void finishBurst();
    void connected(FakeHost& caller, FakeHost& called);
    void disconnected();
    void stepCall(long long nowMs);
    void stepSession(long long nowMs);
    bool sessionIdle(long long nowMs) const;
    FakeHost* reachable(const std::string& call);
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
    int airtimeMs_ = 0;
    std::vector<Tagged> onAir_;     // keyed up, not yet heard
    long long onAirUntilMs_ = -1;
    FakeHost* partner_ = nullptr;

    // A call under way.
    int answerOn_ = 1;
    bool calling_ = false;
    std::string target_;
    int calls_ = 0;
    bool keyed_ = false;
    long long callNextMs_ = 0;

    // The session's bytes, read and waiting to cross.
    struct Crossing
    {
        std::vector<uint8_t> bytes;
        long long atMs;
    };
    std::vector<Crossing> crossing_;
    int sessionRate_ = 0;
    long long lastCrossingMs_ = 0;  // when the last byte read is due across
    size_t waiting_ = 0;
    bool fading_ = false;
    bool disconnectPending_ = false;
    long long lastSessionMs_ = 0;   // bytes last read or carried
    int idleBroadcastMs_ = 0;
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
