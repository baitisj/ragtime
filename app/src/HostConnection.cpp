#include "HostConnection.h"

#include <algorithm>
#include <utility>

#include <wx/time.h>

namespace
{

constexpr int TICK_MS = 100;
// A try that hasn't connected in this long has failed.
constexpr uint64_t CONNECT_TIMEOUT_MS = 5000;

// The session data port, when it won't connect or closes, is tried again
// this often while the other two are up.
constexpr uint64_t DATA_RETRY_MS = 5000;

const int ID_KISS = 1;
const int ID_COMMAND = 2;
const int ID_DATA = 3;

} // namespace

HostConnection::HostConnection(ragtime::link::HostLink& link, EventsFn onEvents)
    : link_(link)
    , onEvents_(std::move(onEvents))
    , timer_(this)
{
    Bind(wxEVT_SOCKET, &HostConnection::onSocket, this);
    Bind(wxEVT_TIMER, &HostConnection::onTimer, this);
    timer_.Start(TICK_MS);
}

HostConnection::~HostConnection()
{
    timer_.Stop();
    for (Port* port : {&kiss_, &command_, &data_})
    {
        if (port->socket != nullptr)
        {
            port->socket->Notify(false);
            port->socket->Destroy();
            port->socket = nullptr;
        }
    }
}

uint64_t HostConnection::nowMs()
{
    return (uint64_t)wxGetUTCTimeMillis().GetValue();
}

std::string HostConnection::address() const
{
    return host_ + ":" + std::to_string(commandPort_);
}

std::string HostConnection::describe() const
{
    switch (state_)
    {
        case State::Off:
            return "Not connected";
        case State::Connecting:
            return "Connecting";
        case State::Connected:
            return "Connected";
        case State::Waiting:
            return "Reconnecting in " + std::to_string(std::max(1, reconnect_.secondsLeft(nowMs()))) + " s";
    }
    return std::string();
}

void HostConnection::start(const std::string& host, int kissPort, int commandPort)
{
    closeBoth(std::string());
    host_ = host;
    kissPort_ = kissPort;
    commandPort_ = commandPort;
    reconnect_.connected();
    error_.clear();
    connectBoth();
}

void HostConnection::stop()
{
    closeBoth(std::string());
    reconnect_.connected();
    state_ = State::Off;
    flush();
}

namespace
{

void openPort(wxEvtHandler& handler, wxSocketClient*& socket, int id, const std::string& host, int number)
{
    socket = new wxSocketClient(wxSOCKET_NOWAIT);
    socket->SetEventHandler(handler, id);
    socket->SetNotify(wxSOCKET_CONNECTION_FLAG | wxSOCKET_INPUT_FLAG | wxSOCKET_OUTPUT_FLAG | wxSOCKET_LOST_FLAG);
    socket->Notify(true);
    wxIPV4address address;
    address.Hostname(wxString::FromUTF8(host));
    address.Service((unsigned short)number);
    socket->Connect(address, false);
}

} // namespace

void HostConnection::connectBoth()
{
    reconnect_.trying();
    state_ = State::Connecting;
    triedMs_ = nowMs();
    openPort(*this, kiss_.socket, ID_KISS, host_, kissPort_);
    openPort(*this, command_.socket, ID_COMMAND, host_, commandPort_);
    // The data port at the same time, so it is up before a session comes.
    connectData();
}

void HostConnection::connectData()
{
    if (data_.socket != nullptr) return;
    dataRetryMs_ = nowMs() + DATA_RETRY_MS;
    openPort(*this, data_.socket, ID_DATA, host_, commandPort_ + 1);
}

void HostConnection::closeData()
{
    bool wasUp = data_.up;
    if (data_.socket != nullptr)
    {
        data_.socket->Notify(false);
        data_.socket->Destroy();
        data_.socket = nullptr;
    }
    data_.up = false;
    data_.out.clear();
    dataRetryMs_ = nowMs() + DATA_RETRY_MS;
    if (wasUp) recorder_.dataUp(false, nowMs());
    if (wasUp && dataSide_.up) dataSide_.up(false);
}

void HostConnection::closeBoth(const std::string& why)
{
    bool wasUp = state_ == State::Connected;
    for (Port* port : {&kiss_, &command_})
    {
        if (port->socket != nullptr)
        {
            port->socket->Notify(false);
            port->socket->Destroy();
            port->socket = nullptr;
        }
        port->up = false;
        port->out.clear();
    }
    closeData();
    if (wasUp)
    {
        recorder_.down(why.empty() ? "closed by Ragtime" : why, nowMs());
        link_.commandDown(nowMs());
    }
    if (!why.empty())
    {
        error_ = why;
        state_ = State::Waiting;
        reconnect_.failed(nowMs());
    }
    flush();
}

void HostConnection::onSocket(wxSocketEvent& event)
{
    if (event.GetId() == ID_DATA)
    {
        if (data_.socket == nullptr || event.GetSocket() != data_.socket) return;
        switch (event.GetSocketEvent())
        {
            case wxSOCKET_CONNECTION:
                data_.up = true;
                recorder_.dataUp(true, nowMs());
                if (dataSide_.up) dataSide_.up(true);
                break;
            case wxSOCKET_INPUT:
            {
                char buffer[4096];
                for (;;)
                {
                    data_.socket->Read(buffer, sizeof buffer);
                    size_t n = data_.socket->LastReadCount();
                    if (n == 0) break;
                    recorder_.dataIn(n, nowMs());
                    if (dataSide_.bytes) dataSide_.bytes((const uint8_t*)buffer, n);
                    if (data_.socket == nullptr) return;
                }
                break;
            }
            case wxSOCKET_OUTPUT:
                write(data_);
                break;
            case wxSOCKET_LOST:
                // Only sessions need it: the group carries on without it.
                closeData();
                break;
        }
        flush();
        return;
    }

    Port& port = event.GetId() == ID_KISS ? kiss_ : command_;
    bool isCommand = &port == &command_;
    if (port.socket == nullptr || event.GetSocket() != port.socket) return;
    const char* name = isCommand ? "command port" : "KISS port";

    switch (event.GetSocketEvent())
    {
        case wxSOCKET_CONNECTION:
            port.up = true;
            if (kiss_.up && command_.up && state_ != State::Connected)
            {
                state_ = State::Connected;
                reconnect_.connected();
                error_.clear();
                recorder_.up(host_ + " KISS " + std::to_string(kissPort_) + " commands " + std::to_string(commandPort_),
                             nowMs());
                link_.commandUp(nowMs());
            }
            break;
        case wxSOCKET_INPUT:
            read(port, isCommand);
            break;
        case wxSOCKET_OUTPUT:
            write(port);
            break;
        case wxSOCKET_LOST:
            closeBoth(state_ == State::Connected ? std::string("data2g-host closed its ") + name
                                                 : std::string("No data2g-host on ") + host_ + " "
                                                       + std::to_string(isCommand ? commandPort_ : kissPort_));
            return;
    }
    flush();
}

void HostConnection::read(Port& port, bool command)
{
    char buffer[4096];
    for (;;)
    {
        port.socket->Read(buffer, sizeof buffer);
        size_t n = port.socket->LastReadCount();
        if (n == 0) break;
        if (command)
        {
            recorder_.commandIn(buffer, n, nowMs());
            link_.commandBytes(buffer, n, nowMs());
        }
        else
        {
            recorder_.kissIn((const uint8_t*)buffer, n, nowMs());
            link_.kissBytes((const uint8_t*)buffer, n, nowMs());
        }
        if (port.socket == nullptr) return;
    }
}

void HostConnection::write(Port& port)
{
    if (port.socket == nullptr || !port.up || port.out.empty()) return;
    port.socket->Write(port.out.data(), (wxUint32)port.out.size());
    size_t n = port.socket->LastWriteCount();
    recordWritten(port, port.out.data(), n);
    port.out.erase(0, n);
}

void HostConnection::recordWritten(const Port& port, const char* bytes, size_t length)
{
    if (length == 0) return;
    if (&port == &command_)
        recorder_.commandOut(bytes, length, nowMs());
    else if (&port == &kiss_)
        recorder_.kissOut((const uint8_t*)bytes, length, nowMs());
    else
        recorder_.dataOut(length, nowMs());
}

void HostConnection::flush()
{
    // What the window does with the events can give HostLink more to
    // write, and more events: go round until it has nothing more.
    if (flushing_)
    {
        return;
    }
    flushing_ = true;
    for (int round = 0; round < 16; round++)
    {
        if (state_ == State::Connected)
        {
            command_.out += link_.takeCommandOutput();
            std::vector<uint8_t> kiss = link_.takeKissOutput();
            kiss_.out.append(kiss.begin(), kiss.end());
            write(command_);
            write(kiss_);
        }
        else
        {
            link_.takeCommandOutput();
            link_.takeKissOutput();
        }
        if (dataSide_.take)
        {
            std::vector<uint8_t> bytes = dataSide_.take();
            if (data_.up) data_.out.append(bytes.begin(), bytes.end());
            write(data_);
        }
        std::vector<ragtime::link::HostLink::Event> events = link_.takeEvents();
        if (events.empty()) break;
        if (onEvents_) onEvents_(events);
    }
    flushing_ = false;
}

void HostConnection::onTimer(wxTimerEvent&)
{
    uint64_t now = nowMs();
    if (state_ == State::Connecting && now - triedMs_ > CONNECT_TIMEOUT_MS)
    {
        closeBoth("No answer from data2g-host at " + host_);
        return;
    }
    if (state_ == State::Waiting && reconnect_.due(now)) connectBoth();
    if (state_ == State::Connected && !data_.up && now >= dataRetryMs_)
    {
        closeData();
        connectData();
    }
    link_.tick(now);
    flush();
}
