#include "RigConnection.h"

#include <algorithm>
#include <utility>

#include "HostConnection.h"

namespace
{

constexpr int TICK_MS = 250;
// A try that hasn't connected in this long has failed.
constexpr uint64_t CONNECT_TIMEOUT_MS = 5000;

} // namespace

RigConnection::RigConnection(std::function<void()> changed)
    : changed_(std::move(changed))
    , timer_(this)
{
    Bind(wxEVT_SOCKET, &RigConnection::onSocket, this);
    Bind(wxEVT_TIMER, &RigConnection::onTimer, this);
}

RigConnection::~RigConnection()
{
    timer_.Stop();
    if (socket_ != nullptr)
    {
        socket_->Notify(false);
        socket_->Destroy();
        socket_ = nullptr;
    }
}

void RigConnection::start(const std::string& host, int port)
{
    close(std::string());
    host_ = host;
    port_ = port;
    running_ = true;
    reconnect_.connected();
    error_.clear();
    timer_.Start(TICK_MS);
    connect();
}

void RigConnection::stop()
{
    close(std::string());
    running_ = false;
    reconnect_.connected();
    timer_.Stop();
    changed();
}

void RigConnection::setKeyed(bool keyed)
{
    reader_.setKeyed(keyed, HostConnection::nowMs());
    write();
}

std::optional<uint64_t> RigConnection::dialHz() const
{
    return reader_.dialHz(HostConnection::nowMs());
}

std::string RigConnection::describe() const
{
    std::string where = "rigctld " + address() + ": ";
    if (!running_) return where + "not read";
    if (up_)
    {
        if (!reader_.problem().empty()) return where + reader_.problem();
        if (!dialHz()) return where + "connected, waiting for the frequency";
        return where + "connected, reading the frequency once a second";
    }
    if (reconnect_.waiting())
    {
        std::string again = "reconnecting in "
                            + std::to_string(std::max(1, reconnect_.secondsLeft(HostConnection::nowMs()))) + " s";
        return where + (error_.empty() ? again : error_ + "; " + again);
    }
    return where + "connecting";
}

void RigConnection::connect()
{
    reconnect_.trying();
    triedMs_ = HostConnection::nowMs();
    socket_ = new wxSocketClient(wxSOCKET_NOWAIT);
    socket_->SetEventHandler(*this);
    socket_->SetNotify(wxSOCKET_CONNECTION_FLAG | wxSOCKET_INPUT_FLAG | wxSOCKET_OUTPUT_FLAG | wxSOCKET_LOST_FLAG);
    socket_->Notify(true);
    wxIPV4address address;
    address.Hostname(wxString::FromUTF8(host_));
    address.Service((unsigned short)port_);
    socket_->Connect(address, false);
}

void RigConnection::close(const std::string& why)
{
    if (socket_ != nullptr)
    {
        socket_->Notify(false);
        socket_->Destroy();
        socket_ = nullptr;
    }
    bool wasUp = up_;
    up_ = false;
    out_.clear();
    if (wasUp) reader_.disconnected();
    if (!why.empty())
    {
        error_ = why;
        reconnect_.failed(HostConnection::nowMs());
    }
    changed();
}

void RigConnection::onSocket(wxSocketEvent& event)
{
    if (socket_ == nullptr || event.GetSocket() != socket_) return;
    switch (event.GetSocketEvent())
    {
        case wxSOCKET_CONNECTION:
            up_ = true;
            error_.clear();
            reconnect_.connected();
            reader_.connected(HostConnection::nowMs());
            break;
        case wxSOCKET_INPUT:
        {
            char buffer[512];
            for (;;)
            {
                socket_->Read(buffer, sizeof buffer);
                size_t n = socket_->LastReadCount();
                if (n == 0) break;
                reader_.bytes(buffer, n, HostConnection::nowMs());
            }
            break;
        }
        case wxSOCKET_OUTPUT:
            break;
        case wxSOCKET_LOST:
            close(up_ ? std::string("rigctld closed the connection")
                      : "no rigctld on " + address());
            return;
    }
    write();
    changed();
}

void RigConnection::write()
{
    out_ += reader_.takeOutput();
    if (socket_ == nullptr || !up_ || out_.empty()) return;
    socket_->Write(out_.data(), (wxUint32)out_.size());
    out_.erase(0, socket_->LastWriteCount());
}

void RigConnection::changed()
{
    seenVersion_ = reader_.version();
    if (changed_) changed_();
}

void RigConnection::onTimer(wxTimerEvent&)
{
    if (!running_) return;
    uint64_t now = HostConnection::nowMs();
    if (socket_ != nullptr && !up_ && now - triedMs_ > CONNECT_TIMEOUT_MS)
    {
        close("no answer from rigctld at " + address());
        return;
    }
    if (socket_ == nullptr && reconnect_.due(now)) connect();
    reader_.tick(now);
    write();
    if (reader_.version() != seenVersion_) changed();
}
