#include "RigConnection.h"

#include <algorithm>
#include <utility>

#include "HostConnection.h"
#include "ragtime/radio/Bands.h"

namespace
{

constexpr int TICK_MS = 250;
// An address that hasn't connected in this long has failed.
constexpr uint64_t CONNECT_TIMEOUT_MS = 3000;

bool loopback(const std::string& host)
{
    return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

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
    resolve();
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
        if (connectedTo_ != address()) where = "rigctld " + address() + " (" + connectedTo_ + "): ";
        if (!reader_.problem().empty()) return where + reader_.problem();
        if (!dialHz()) return where + "connected, waiting for the frequency";
        return where + "connected, reading the frequency once a second" + (vfoMode() ? " (--vfo)" : "");
    }
    if (reconnect_.waiting())
    {
        std::string again = "reconnecting in "
                            + std::to_string(std::max(1, reconnect_.secondsLeft(HostConnection::nowMs()))) + " s";
        return where + (error_.empty() ? again : error_ + "; " + again);
    }
    return where + "connecting";
}

void RigConnection::resolve()
{
    addresses_.clear();
    names_.clear();
    at_ = first_ = 0;
    std::string host = host_;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    auto add = [&](std::unique_ptr<wxIPaddress> address, bool v6) {
        address->Service((unsigned short)port_);
        std::string ip(address->IPAddress().utf8_str());
        std::string name = (v6 ? "[" + ip + "]" : ip) + ":" + std::to_string(port_);
        for (const std::string& had : names_)
        {
            if (had == name) return;
        }
        names_.push_back(name);
        addresses_.push_back(std::move(address));
    };
    // Looked up afresh on each start: blocking, but only for a name.
    auto v4 = std::make_unique<wxIPV4address>();
    if (v4->Hostname(wxString::FromUTF8(loopback(host) ? std::string("127.0.0.1") : host))) add(std::move(v4), false);
#if wxUSE_IPV6
    auto v6 = std::make_unique<wxIPV6address>();
    if (v6->Hostname(wxString::FromUTF8(loopback(host) ? std::string("::1") : host))) add(std::move(v6), true);
#endif
}

void RigConnection::connect()
{
    reconnect_.trying();
    connectTicks_ = 0;
    if (addresses_.empty())
    {
        failures_.clear();
        close("can't find " + host_);
        return;
    }
    if (at_ == first_) failures_.clear();
    socket_ = new wxSocketClient(wxSOCKET_NOWAIT);
    socket_->SetEventHandler(*this);
    socket_->SetNotify(wxSOCKET_CONNECTION_FLAG | wxSOCKET_INPUT_FLAG | wxSOCKET_OUTPUT_FLAG | wxSOCKET_LOST_FLAG);
    socket_->Notify(true);
    socket_->Connect(*addresses_[at_], false);
}

void RigConnection::next(const std::string& why)
{
    failures_.push_back(names_[at_] + " " + why);
    at_ = (at_ + 1) % addresses_.size();
    if (at_ != first_)
    {
        if (socket_ != nullptr)
        {
            socket_->Notify(false);
            socket_->Destroy();
            socket_ = nullptr;
        }
        connect();
        return;
    }
    // Every address failed: "no rigctld: 127.0.0.1:4532 refused, [::1]:4532 refused".
    std::string all;
    for (const std::string& f : failures_) all += (all.empty() ? "" : ", ") + f;
    close("no rigctld: " + all);
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
        note("rigctld " + address() + ": " + why);
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
            first_ = at_;
            connectedTo_ = names_[at_];
            error_.clear();
            notedProblem_.clear();
            dialNoted_ = false;
            note("rigctld " + address() + ": connected to " + connectedTo_);
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
            if (up_)
                close("rigctld at " + connectedTo_ + " closed the connection");
            else
                next("refused");
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
    if (up_)
    {
        if (reader_.problem() != notedProblem_)
        {
            notedProblem_ = reader_.problem();
            if (!notedProblem_.empty()) note("rigctld " + connectedTo_ + ": " + notedProblem_);
        }
        std::optional<uint64_t> hz = dialHz();
        if (hz && !dialNoted_)
        {
            dialNoted_ = true;
            note("rigctld " + connectedTo_ + ": the dial is at " + ragtime::radio::formatMHz(*hz)
                 + (vfoMode() ? " (rigctld started with --vfo)" : ""));
        }
    }
    if (changed_) changed_();
}

void RigConnection::note(const std::string& text)
{
    if (!note_ || text == noted_) return;
    noted_ = text;
    note_(text);
}

void RigConnection::onTimer(wxTimerEvent&)
{
    if (!running_) return;
    uint64_t now = HostConnection::nowMs();
    // Counted in ticks, not from the clock: a try begun while the window
    // was still being built (or under valgrind) gets its full time once
    // the event loop runs, as the socket's own events do.
    if (socket_ != nullptr && !up_ && ++connectTicks_ * (uint64_t)TICK_MS > CONNECT_TIMEOUT_MS)
    {
        next("didn't answer");
        return;
    }
    if (socket_ == nullptr && reconnect_.due(now)) connect();
    reader_.tick(now);
    write();
    if (reader_.version() != seenVersion_) changed();
}
