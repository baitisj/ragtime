#ifndef RAGTIME_RIG_CONNECTION_H
#define RAGTIME_RIG_CONNECTION_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <wx/event.h>
#include <wx/socket.h>
#include <wx/timer.h>

#include "ragtime/link/Reconnect.h"
#include "ragtime/radio/DialReader.h"

// The socket to rigctld, carrying bytes to and from the socket-free
// DialReader, which only ever asks the radio's frequency; tried again 2,
// 4, 8, 15 and then every 30 s while rigctld isn't there. Ragtime never
// starts rigctld, and never sets the radio: the operator's rigctld is
// often shared with other programs.
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

private:
    void connect();
    void close(const std::string& why);
    void onSocket(wxSocketEvent& event);
    void onTimer(wxTimerEvent& event);
    void write();
    void changed();

    std::function<void()> changed_;
    ragtime::radio::DialReader reader_;
    ragtime::link::Reconnect reconnect_;
    wxTimer timer_;
    wxSocketClient* socket_ = nullptr;
    bool up_ = false;
    bool running_ = false;
    uint64_t triedMs_ = 0;
    std::string out_;
    std::string host_;
    int port_ = 0;
    std::string error_;
    uint64_t seenVersion_ = 0;
};

#endif
