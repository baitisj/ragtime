#ifndef RAGTIME_HOST_CONNECTION_H
#define RAGTIME_HOST_CONNECTION_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/socket.h>
#include <wx/timer.h>

#include "ragtime/link/HostLink.h"
#include "ragtime/link/Reconnect.h"

// The sockets to data2g-host: its KISS and command ports, carrying bytes
// to and from the socket-free HostLink, and trying again 2, 4, 8, 15 and
// then every 30 s while the host isn't there; and the session data port
// (command + 1), carrying a session's byte stream for the Session tab,
// tried again every 5 s on its own while the other two are up. Ragtime
// never starts data2g-host; the operator runs it.
class HostConnection : public wxEvtHandler
{
public:
    enum class State
    {
        Off,            // no callsign yet, or stopped
        Connecting,
        Connected,
        Waiting,        // to try again
    };

    using EventsFn = std::function<void(const std::vector<ragtime::link::HostLink::Event>&)>;

    // The session data port's side: what arrives, what to write, and
    // whether it is up.
    struct DataSide
    {
        std::function<void(const uint8_t* bytes, size_t length)> bytes;
        std::function<std::vector<uint8_t>()> take;
        std::function<void(bool up)> up;
    };

    HostConnection(ragtime::link::HostLink& link, EventsFn onEvents);
    ~HostConnection() override;

    // Connects to host:kissPort and host:commandPort, closing any
    // connection first, and keeps trying until stop().
    void start(const std::string& host, int kissPort, int commandPort);
    void stop();

    void setDataSide(DataSide side) { dataSide_ = std::move(side); }
    bool dataUp() const { return data_.up; }

    State state() const { return state_; }
    // "Connected", "Connecting", "Reconnecting in 8 s", "Not connected".
    std::string describe() const;
    // Why the last try failed, for a tooltip.
    const std::string& lastError() const { return error_; }
    std::string address() const;

    // Writes what HostLink has for the host and passes on its events; the
    // window calls it after anything it asks of HostLink.
    void flush();

    static uint64_t nowMs();

private:
    struct Port
    {
        wxSocketClient* socket = nullptr;
        bool up = false;
        std::string out;
    };

    void connectBoth();
    void connectData();
    void closeData();
    void closeBoth(const std::string& why);
    void onSocket(wxSocketEvent& event);
    void onTimer(wxTimerEvent& event);
    void read(Port& port, bool command);
    void write(Port& port);

    ragtime::link::HostLink& link_;
    EventsFn onEvents_;
    wxTimer timer_;
    ragtime::link::Reconnect reconnect_;
    Port kiss_, command_, data_;
    DataSide dataSide_;
    uint64_t dataRetryMs_ = 0;  // the data port is tried again then
    std::string host_;
    int kissPort_ = 0;
    int commandPort_ = 0;
    State state_ = State::Off;
    bool flushing_ = false;
    uint64_t triedMs_ = 0;
    std::string error_;
};

#endif
