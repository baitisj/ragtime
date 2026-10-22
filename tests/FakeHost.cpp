// The stand-in for data2g-host: see FakeHost.h.
#include "FakeHost.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ragtime/link/Data2GLink.h"

using namespace ragtime::link;

namespace ragtime_test
{

namespace
{

// A listening socket on 127.0.0.1:port (0: any free port), non-blocking.
int listenOn(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(fd, 4) != 0)
    {
        ::close(fd);
        return -1;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

int boundPort(int fd)
{
    sockaddr_in addr{};
    socklen_t length = sizeof(addr);
    ::getsockname(fd, (sockaddr*)&addr, &length);
    return ntohs(addr.sin_port);
}

void closeFd(int& fd)
{
    if (fd >= 0) ::close(fd);
    fd = -1;
}

// Takes a new client, replacing the one before, and reads what has come;
// false if the client closed.
bool service(int& listener, int& client, std::vector<uint8_t>& in)
{
    int fresh = ::accept(listener, nullptr, nullptr);
    if (fresh >= 0)
    {
        closeFd(client);
        client = fresh;
        in.clear();
    }
    if (client < 0) return true;
    uint8_t buffer[4096];
    for (;;)
    {
        ssize_t n = ::recv(client, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0)
        {
            in.insert(in.end(), buffer, buffer + n);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        {
            closeFd(client);
            return false;
        }
        if (errno != EINTR) return true;
    }
}

std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    std::string word;
    for (char c : line)
    {
        if (c == ' ')
        {
            if (!word.empty()) out.push_back(word);
            word.clear();
        }
        else
        {
            word.push_back(c);
        }
    }
    if (!word.empty()) out.push_back(word);
    return out;
}

} // namespace

FakeHost::FakeHost(FakeAir& air, int groupPort) : air_(air), groupPort_(groupPort)
{
    kiss_.listener = listenOn(0);
    if (kiss_.listener < 0) throw std::runtime_error("fake host: no KISS port");
    kissPort_ = boundPort(kiss_.listener);

    // The data port is the command port + 1, so the two are taken together.
    std::random_device random;
    for (int attempt = 0; attempt < 100 && commandPort_ == 0; attempt++)
    {
        int port = 20000 + (int)(random() % 40000);
        command_.listener = listenOn(port);
        if (command_.listener < 0) continue;
        data_.listener = listenOn(port + 1);
        if (data_.listener < 0)
        {
            closeFd(command_.listener);
            continue;
        }
        commandPort_ = port;
    }
    if (commandPort_ == 0) throw std::runtime_error("fake host: no command and data ports");
    air_.hosts_.push_back(this);
}

FakeHost::~FakeHost()
{
    if (partner_ != nullptr) partner_->partner_ = nullptr;
    air_.hosts_.erase(std::remove(air_.hosts_.begin(), air_.hosts_.end(), this), air_.hosts_.end());
    for (Port* port : {&kiss_, &command_, &data_})
    {
        closeFd(port->client);
        closeFd(port->listener);
    }
}

void FakeHost::script(const std::string& prefix, const std::vector<std::string>& replies)
{
    scripts_[prefix] = replies;
}

void FakeHost::say(const std::string& line)
{
    reply(line);
}

void FakeHost::closeCommand()
{
    closeFd(command_.client);
    lineIn_.clear();
}

void FakeHost::pollFds(std::vector<pollfd>& fds)
{
    for (Port* port : {&kiss_, &command_, &data_})
    {
        fds.push_back({port->listener, POLLIN, 0});
        if (port->client >= 0) fds.push_back({port->client, POLLIN, 0});
    }
}

void FakeHost::reply(const std::string& line)
{
    std::string text = line + "\r";
    writeTo(command_, std::vector<uint8_t>(text.begin(), text.end()));
}

void FakeHost::writeTo(Port& port, const std::vector<uint8_t>& bytes)
{
    size_t at = 0;
    while (port.client >= 0 && at < bytes.size())
    {
        ssize_t n = ::send(port.client, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
        if (n > 0)
        {
            at += (size_t)n;
        }
        else if (errno != EINTR)
        {
            closeFd(port.client);
        }
    }
}

void FakeHost::serviceCommand()
{
    bool had = command_.client >= 0;
    service(command_.listener, command_.client, command_.in);
    if (had && command_.client < 0) lineIn_.clear();
    for (uint8_t b : command_.in)
    {
        if (b == '\r' || b == '\n')
        {
            if (!lineIn_.empty()) onCommand(lineIn_);
            lineIn_.clear();
        }
        else
        {
            lineIn_.push_back((char)b);
        }
    }
    command_.in.clear();
}

void FakeHost::onCommand(const std::string& line)
{
    commands_.push_back(line);
    for (auto it = scripts_.begin(); it != scripts_.end(); ++it)
    {
        if (line.compare(0, it->first.size(), it->first) == 0)
        {
            for (const std::string& r : it->second) reply(r);
            scripts_.erase(it);
            return;
        }
    }

    std::vector<std::string> w = words(line);
    if (w.empty()) return;
    const std::string& verb = w[0];
    if (verb == "MYCALL" && w.size() >= 2)
    {
        call_ = w[1];
        reply("OK");
    }
    else if (verb == "LISTEN" && w.size() >= 2)
    {
        listening_ = w[1] == "ON";
        reply("OK");
    }
    else if (verb == "CHAT" || verb == "BW500" || verb == "BW2300")
    {
        reply("OK");
    }
    else if (verb == "BCAST" && w.size() >= 5 && w[1] == "OPEN" && w[3] == "FROM")
    {
        from_ = w[4];
        groupOpen_ = true;
        reply("BCAST PORT " + std::to_string(groupPort_));
    }
    else if (verb == "BCAST" && w.size() >= 4 && w[1] == "MODE")
    {
        mode_ = w[3];
        reply("OK");
    }
    else if (verb == "MODES")
    {
        for (const ModeInfo& m : knownModes())
        {
            char tail[64];
            std::snprintf(tail, sizeof(tail), " %.2f %.2f", m.secondsAtOne, m.secondsAtMax);
            reply("MODE " + m.name + " " + std::to_string(m.bandwidthHz) + " " +
                  std::to_string(m.bytesPerCodeword) + " " + std::to_string(m.maxCodewords) + tail);
        }
        reply("OK");
    }
    else if (verb == "CONNECT" && w.size() >= 3)
    {
        reply("OK");
        FakeHost* called = nullptr;
        for (FakeHost* h : air_.hosts_)
        {
            if (h != this && h->call_ == w[2] && h->listening_ && h->partner_ == nullptr) called = h;
        }
        if (called != nullptr && partner_ == nullptr)
        {
            connected(*this, *called);
        }
        else
        {
            reply("DISCONNECTED");
        }
    }
    else if (verb == "DISCONNECT" || verb == "ABORT")
    {
        reply("OK");
        disconnected();
    }
    else
    {
        reply("WRONG");
    }
}

void FakeHost::connected(FakeHost& caller, FakeHost& called)
{
    caller.partner_ = &called;
    called.partner_ = &caller;
    std::string line = "CONNECTED " + caller.call_ + " " + called.call_ + " 2300";
    caller.reply(line);
    called.reply(line);
}

void FakeHost::disconnected()
{
    if (partner_ == nullptr) return;
    FakeHost* other = partner_;
    partner_ = nullptr;
    other->partner_ = nullptr;
    reply("DISCONNECTED");
    other->reply("DISCONNECTED");
}

void FakeHost::serviceKiss()
{
    int had = kiss_.client;
    service(kiss_.listener, kiss_.client, kiss_.in);
    if (kiss_.client != had) kissDecoder_.reset();
    if (kiss_.in.empty()) return;
    std::vector<KissFrame> frames;
    kissDecoder_.feed(kiss_.in.data(), kiss_.in.size(), frames);
    kiss_.in.clear();
    for (KissFrame& f : frames)
    {
        if (f.port != groupPort_ || !groupOpen_) continue;
        if (f.command == KISS_ACKMODE && f.payload.size() >= 2)
        {
            uint16_t tag = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            burst_.push_back({tag, std::vector<uint8_t>(f.payload.begin() + 2, f.payload.end())});
        }
        else if (f.command == KISS_DATA)
        {
            burst_.push_back({0, f.payload});
        }
    }
}

void FakeHost::serviceData()
{
    service(data_.listener, data_.client, data_.in);
    if (data_.in.empty()) return;
    std::vector<uint8_t> bytes;
    bytes.swap(data_.in);
    if (partner_ == nullptr) return; // no session: nowhere for them to go
    reply("BUFFER " + std::to_string(bytes.size()));
    partner_->writeTo(partner_->data_, bytes);
    reply("BUFFER 0");
}

void FakeHost::sendBurst()
{
    std::vector<Tagged> burst;
    burst.swap(burst_);
    if (dropNext_)
    {
        dropNext_ = false;
        reply("BCAST " + std::to_string(groupPort_) + " DROPPED " + std::to_string(burst.size()));
        return;
    }
    reply("PTT ON");
    reply("MODE " + mode_);
    for (FakeHost* h : air_.hosts_)
    {
        if (h == this || !h->groupOpen_) continue;
        h->reply("BCAST " + std::to_string(h->groupPort_) + " HEARD " + from_);
        for (const Tagged& t : burst) h->writeTo(h->kiss_, kissEncode(h->groupPort_, t.frame));
    }
    for (const Tagged& t : burst)
    {
        if (t.tag != 0) writeTo(kiss_, kissEncodeAckMode(groupPort_, t.tag, {}));
    }
    reply("PTT OFF");
    burstsSent_++;
}

void FakeAir::step(int timeoutMs)
{
    std::vector<pollfd> fds;
    for (FakeHost* h : hosts_) h->pollFds(fds);
    ::poll(fds.data(), fds.size(), timeoutMs);
    for (FakeHost* h : hosts_)
    {
        h->serviceCommand();
        h->serviceKiss();
        h->serviceData();
    }
    // A session holds the group: its bursts wait for it to end.
    for (FakeHost* h : hosts_)
    {
        if (!h->burst_.empty() && !h->holdBursts_ && h->partner_ == nullptr) h->sendBurst();
    }
}

} // namespace ragtime_test
