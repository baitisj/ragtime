// The stand-in for data2g-host: see FakeHost.h.
#include "FakeHost.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>

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

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

FakeHost::FakeHost(FakeAir& air, int groupPort) : FakeHost(air, groupPort, 0, 0)
{
}

FakeHost::FakeHost(FakeAir& air, int groupPort, int kissPort, int commandPort) : air_(air), groupPort_(groupPort)
{
    kiss_.listener = listenOn(kissPort);
    if (kiss_.listener < 0) throw std::runtime_error("fake host: no KISS port");
    kissPort_ = boundPort(kiss_.listener);

    // The data port is the command port + 1, so the two are taken together.
    if (commandPort != 0)
    {
        command_.listener = listenOn(commandPort);
        data_.listener = command_.listener >= 0 ? listenOn(commandPort + 1) : -1;
        if (data_.listener < 0)
        {
            closeFd(command_.listener);
            closeFd(kiss_.listener);
            throw std::runtime_error("fake host: command or data port taken");
        }
        commandPort_ = commandPort;
    }
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
    if (partner_ != nullptr)
    {
        partner_->partner_ = nullptr;
        partner_->reply(partner_->disconnectedLine("LINK_LOST"));
    }
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
    if (had && command_.client < 0)
    {
        lineIn_.clear();
        // The client that asked for these has gone.
        data2g_ = false;
        idleTimeoutS_ = DEFAULT_IDLE_TIMEOUT_S;
    }
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
    else if (verb == "DATA2G" && w.size() == 2 && cuts_)
    {
        data2g_ = w[1] == "ON";
        reply("OK");
    }
    else if ((verb == "TIMEOUT" || verb == "TXPOWER") && w.size() <= 2 && data2g_ && reasons_)
    {
        double& value = verb == "TIMEOUT" ? idleTimeoutS_ : txPowerDb_;
        if (w.size() == 1)
        {
            char text[48];
            std::snprintf(text, sizeof text, "%s %.1f", verb.c_str(), value);
            reply(text);
            return;
        }
        char* end = nullptr;
        double v = std::strtod(w[1].c_str(), &end);
        bool ok = end != nullptr && *end == '\0' && std::isfinite(v)
                  && (verb == "TIMEOUT" ? v > 0.0 : v >= -60.0 && v <= 20.0);
        if (ok) value = v;
        reply(ok ? "OK" : "WRONG");
    }
    else if (verb == "CLEANTXBUFFER" && w.size() == 2 && data2g_)
    {
        uint64_t upto = std::strtoull(w[1].c_str(), nullptr, 10);
        if (partner_ == nullptr)
        {
            reply("CLEANTXBUFFER FAILED ");
        }
        else if (waiting_ == 0)
        {
            reply("CLEANTXBUFFER BUFFEREMPTY");
        }
        else if (upto < sessionRead_)
        {
            // Only all of it, here.
            reply("CLEANTXBUFFER LATE");
        }
        else
        {
            std::string from = std::to_string(sessionRead_ - waiting_);
            cutBytes_ += waiting_;
            crossing_.clear();
            waiting_ = 0;
            lastCrossingMs_ = 0;
            reply("CLEANTXBUFFER OK " + from + " " + std::to_string(sessionRead_) + " " + from + " " + from);
            reply("BUFFER 0");
            if (disconnectPending_) disconnected("CLIENT", "PEER");
        }
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
    else if (verb == "SN" && reportsSnr_ && partner_ != nullptr && partnerSnr_)
    {
        reply("SN" + snrSuffix(*partnerSnr_));
    }
    else if (verb == "CONNECT" && w.size() >= 3)
    {
        reply("OK");
        if (partner_ != nullptr || calling_)
        {
            reply("DISCONNECTED");
        }
        else
        {
            calling_ = true;
            target_ = w[2];
            calls_ = 0;
            keyed_ = false;
            callNextMs_ = nowMs();
            stepCall(nowMs());
        }
    }
    else if (verb == "DISCONNECT" || verb == "ABORT")
    {
        reply("OK");
        if (calling_)
        {
            calling_ = false;
            if (keyed_) reply("PTT OFF");
            keyed_ = false;
            reply(disconnectedLine(verb == "ABORT" ? "ABORT" : "CLIENT"));
        }
        else if (verb == "DISCONNECT" && waiting_ > 0)
        {
            // DISCONNECT waits for what is still to cross.
            disconnectPending_ = true;
        }
        else if (verb == "ABORT")
        {
            // No DISC goes: the far end loses the link.
            disconnected("ABORT", "LINK_LOST");
        }
        else
        {
            disconnected("CLIENT", "PEER");
        }
    }
    else
    {
        reply("WRONG");
    }
}

FakeHost* FakeHost::reachable(const std::string& call)
{
    for (FakeHost* h : air_.hosts_)
    {
        if (h != this && h->call_ == call && h->listening_ && h->partner_ == nullptr && !h->calling_) return h;
    }
    return nullptr;
}

void FakeHost::stepCall(long long now)
{
    // With no airtime the calls all happen at once.
    for (int i = 0; i < 16 && calling_ && now >= callNextMs_; i++)
    {
        if (!keyed_)
        {
            reply("PTT ON");
            keyed_ = true;
            callNextMs_ = now + airtimeMs_;
            continue;
        }
        reply("PTT OFF");
        keyed_ = false;
        calls_++;
        FakeHost* called = reachable(target_);
        // A host on the air that won't take it says so at the first call
        // it hears.
        FakeHost* there = nullptr;
        for (FakeHost* h : air_.hosts_)
        {
            if (h != this && h->call_ == target_) there = h;
        }
        if (called != nullptr && calls_ >= answerOn_)
        {
            calling_ = false;
            connected(*this, *called);
        }
        else if (called == nullptr && there != nullptr && calls_ >= answerOn_)
        {
            calling_ = false;
            bool busy = there->partner_ != nullptr || there->calling_;
            reply(disconnectedLine(busy ? "BUSY" : "NOT_LISTENING"));
        }
        else if (calls_ >= 5)
        {
            calling_ = false;
            reply(disconnectedLine("NO_ANSWER"));
        }
        else
        {
            callNextMs_ = now + airtimeMs_;
        }
    }
}

void FakeHost::connected(FakeHost& caller, FakeHost& called)
{
    caller.partner_ = &called;
    called.partner_ = &caller;
    for (FakeHost* h : {&caller, &called})
    {
        h->crossing_.clear();
        h->waiting_ = 0;
        h->sessionRead_ = 0;
        h->disconnectPending_ = false;
        h->lastSessionMs_ = nowMs();
        h->partnerSnr_.reset();
    }
    std::string line = "CONNECTED " + caller.call_ + " " + called.call_ + " 2300";
    caller.reply(line);
    called.reply(line);
}

void FakeHost::disconnected(const std::string& why, const std::string& whyThere)
{
    if (partner_ == nullptr) return;
    FakeHost* other = partner_;
    partner_ = nullptr;
    other->partner_ = nullptr;
    for (FakeHost* h : {this, other})
    {
        h->crossing_.clear();
        h->waiting_ = 0;
        h->disconnectPending_ = false;
    }
    reply(disconnectedLine(why));
    other->reply(other->disconnectedLine(whyThere));
}

std::string FakeHost::disconnectedLine(const std::string& why) const
{
    return data2g_ && reasons_ ? "DISCONNECTED " + why : std::string("DISCONNECTED");
}

void FakeHost::stepIdle(long long now)
{
    if (partner_ == nullptr || waiting_ > 0 || partner_->waiting_ > 0) return;
    long long quietMs = now - std::max(lastSessionMs_, partner_->lastSessionMs_);
    // The host whose timer runs out first hangs up.
    FakeHost* first = idleTimeoutS_ <= partner_->idleTimeoutS_ ? this : partner_;
    if (quietMs < (long long)(first->idleTimeoutS_ * 1000.0)) return;
    first->disconnected("IDLE", "PEER");
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
    waiting_ += bytes.size();
    sessionRead_ += bytes.size();
    reply("BUFFER " + std::to_string(waiting_));
    lastSessionMs_ = nowMs();
    long long at = nowMs() + airtimeMs_;
    if (sessionRate_ <= 0)
    {
        crossing_.push_back({std::move(bytes), at});
    }
    else
    {
        // In pieces of a quarter second each, one after another.
        size_t piece = std::max<size_t>(1, (size_t)sessionRate_ / 4);
        at = std::max(at, lastCrossingMs_);
        for (size_t i = 0; i < bytes.size(); i += piece)
        {
            size_t n = std::min(piece, bytes.size() - i);
            at += (long long)(n * 1000 / (size_t)sessionRate_);
            crossing_.push_back({std::vector<uint8_t>(bytes.begin() + (long)i, bytes.begin() + (long)(i + n)), at});
        }
        lastCrossingMs_ = at;
    }
    stepSession(nowMs());
}

void FakeHost::stepSession(long long now)
{
    // A fade is the path's: nothing crosses either way.
    if (partner_ == nullptr || fading_ || partner_->fading_) return;
    size_t carried = 0;
    while (carried < crossing_.size() && crossing_[carried].atMs <= now)
    {
        Crossing& c = crossing_[carried++];
        partner_->writeTo(partner_->data_, c.bytes);
        waiting_ -= c.bytes.size();
        // The partner heard our data, and we its acknowledgement.
        partner_->partnerSnr_ = heardAtDb();
        partnerSnr_ = partner_->heardAtDb();
        lastSessionMs_ = partner_->lastSessionMs_ = now;
    }
    if (carried == 0) return;
    crossing_.erase(crossing_.begin(), crossing_.begin() + (long)carried);
    reply("BUFFER " + std::to_string(waiting_));
    if (disconnectPending_ && waiting_ == 0) disconnected("CLIENT", "PEER");
}

double FakeHost::heardAtDb()
{
    if (jitterDb_ <= 0.0) return signalDb_;
    return signalDb_ + std::uniform_real_distribution<double>(-jitterDb_, jitterDb_)(snrRandom_);
}

std::string FakeHost::snrSuffix(double dB) const
{
    char text[32];
    std::snprintf(text, sizeof text, " %.1f", dB);
    return text;
}

bool FakeHost::sessionIdle(long long now) const
{
    if (partner_ == nullptr) return !calling_;
    if (idleBroadcastMs_ <= 0 || waiting_ > 0 || partner_->waiting_ > 0) return false;
    return now >= std::max(lastSessionMs_, partner_->lastSessionMs_) + idleBroadcastMs_;
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
    onAir_ = std::move(burst);
    onAirUntilMs_ = nowMs() + airtimeMs_;
    if (airtimeMs_ <= 0) finishBurst();
}

void FakeHost::finishBurst()
{
    onAirUntilMs_ = -1;
    std::vector<Tagged> burst;
    burst.swap(onAir_);
    for (FakeHost* h : air_.hosts_)
    {
        if (h == this || !h->groupOpen_) continue;
        std::vector<const Tagged*> heard;
        for (const Tagged& t : burst)
        {
            if (h->loss_ > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(h->lossRandom_) < h->loss_)
                h->framesLost_++;
            else
                heard.push_back(&t);
        }
        if (heard.empty() && !burst.empty())
        {
            h->reply("BCAST " + std::to_string(h->groupPort_) + " LOST 1");
            continue;
        }
        h->reply("BCAST " + std::to_string(h->groupPort_) + " HEARD " + from_
                 + (h->reportsSnr_ ? h->snrSuffix(heardAtDb()) : std::string()));
        for (const Tagged* t : heard) h->writeTo(h->kiss_, kissEncode(h->groupPort_, t->frame));
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
    for (FakeHost* h : hosts_)
    {
        h->stepCall(nowMs());
        h->stepSession(nowMs());
        h->stepIdle(nowMs());
    }
    // A session holds the group: its bursts wait for it to end, or to go
    // idle.
    for (FakeHost* h : hosts_)
    {
        if (h->onAirUntilMs_ >= 0)
        {
            // One burst on the air at a time.
            if (nowMs() >= h->onAirUntilMs_) h->finishBurst();
            continue;
        }
        if (!h->burst_.empty() && !h->holdBursts_ && h->sessionIdle(nowMs())) h->sendBurst();
    }
}

} // namespace ragtime_test
