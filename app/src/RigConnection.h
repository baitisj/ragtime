#ifndef RAGTIME_RIG_CONNECTION_H
#define RAGTIME_RIG_CONNECTION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/socket.h>
#include <wx/timer.h>

#include "ragtime/link/Reconnect.h"
#include "ragtime/radio/DialReader.h"

// The socket to rigctld, carrying bytes to and from the socket-free
// DialReader, which only ever asks the radio's frequency (and whether
// rigctld wants VFOs named); tried again 2, 4, 8, 15 and then every 30 s
// while rigctld isn't there. Ragtime never starts rigctld, and never sets
// the radio: the operator's rigctld is often shared with other programs.
//
// rigctld may listen on IPv4 or IPv6 alone (rigctld -T ::1, or a
// "localhost" that means ::1), so each try goes through every address the
// host has, IPv4 first, and for 127.0.0.1, ::1 or localhost both loopback
// addresses; the one that answered is tried first next time.
class RigConnection : public wxEvtHandler
{
public:
    // Called when the reading or the connection changes.
    explicit RigConnection(std::function<void()> changed);
    ~RigConnection() override;

    // Connects to host:port, closing any connection first, and keeps
    // trying until stop().
    void start(const std::string& host, int port);
    void stop();
    bool running() const { return running_; }

    // Our transmitter is keyed: no asking meanwhile.
    void setKeyed(bool keyed);

    std::optional<uint64_t> dialHz() const;
    // "rigctld localhost:4532: connected", "... reconnecting in 8 s", and
    // why the last try failed or the reading is missing, for a tooltip.
    std::string describe() const;
    std::string address() const { return host_ + ":" + std::to_string(port_); }

    // Called with a line for the flight recorder when rigctld connects,
    // fails, answers with an error, or first tells the dial; never the
    // same line twice in a row.
    void setNote(std::function<void(const std::string&)> note) { note_ = std::move(note); }

    // For Settings' Test: connected and has the dial, or tried every
    // address and failed; why, and the address that answered.
    bool up() const { return up_; }
    bool failed() const { return !up_ && reconnect_.waiting(); }
    const std::string& error() const { return error_; }
    const std::string& problem() const { return reader_.problem(); }
    const std::string& connectedTo() const { return connectedTo_; }
    bool vfoMode() const { return reader_.vfoMode(); }

    // The transmit meters, asked while keyed: which rigctld answers, and
    // the readings since last time.
    bool readsAlc() const { return up_ && reader_.reads(ragtime::radio::DialReader::Meter::Alc); }
    bool readsSwr() const { return up_ && reader_.reads(ragtime::radio::DialReader::Meter::Swr); }
    bool readsPower() const { return up_ && reader_.readsPower(); }
    bool readsMeters() const { return up_ && reader_.readsAny(); }
    std::vector<ragtime::radio::MeterReading> takeReadings() { return reader_.takeReadings(); }
    // The S-meter, read between bursts, in dB over S9.
    bool readsStrength() const { return up_ && reader_.readsStrength(); }
    std::vector<double> takeStrength() { return reader_.takeStrength(); }
    // The SWR trip is on: the SWR every other question while keyed.
    void setSwrWatch(bool on) { reader_.setSwrWatch(on); }

private:
    void resolve();
    void connect();
    // This address failed: the next one, or give up till the next try.
    void next(const std::string& why);
    void close(const std::string& why);
    void onSocket(wxSocketEvent& event);
    void onTimer(wxTimerEvent& event);
    void write();
    void changed();
    void note(const std::string& text);

    std::function<void()> changed_;
    std::function<void(const std::string&)> note_;
    std::string noted_;             // the last line noted
    std::string notedProblem_;
    bool dialNoted_ = false;
    ragtime::radio::DialReader reader_;
    ragtime::link::Reconnect reconnect_;
    wxTimer timer_;
    wxSocketClient* socket_ = nullptr;
    bool up_ = false;
    bool running_ = false;
    int connectTicks_ = 0;          // ticks since this address was tried
    std::string out_;
    std::string host_;
    int port_ = 0;
    std::vector<std::unique_ptr<wxSockAddress>> addresses_;
    std::vector<std::string> names_;    // "127.0.0.1:4532", "[::1]:4532"
    size_t at_ = 0;                     // the address being tried
    size_t first_ = 0;                  // where a try starts
    std::vector<std::string> failures_; // this try's
    std::string connectedTo_;
    std::string error_;
    uint64_t seenVersion_ = 0;
};

#endif
