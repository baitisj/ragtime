// Plays a flight recording's host side back: see ReplayHost.h.
#include "ReplayHost.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using ragtime::link::FlightLine;
using Kind = ragtime::link::FlightLine::Kind;

namespace ragtime_test
{

namespace
{

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

// A new client from the listener, if one is waiting (-1 if not).
int acceptFrom(int listener)
{
    return listener >= 0 ? ::accept(listener, nullptr, nullptr) : -1;
}

// Reads what has come; false if the client closed.
bool readFrom(int fd, std::vector<uint8_t>& in)
{
    uint8_t buffer[4096];
    for (;;)
    {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0)
        {
            in.insert(in.end(), buffer, buffer + n);
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

// The ports close this long after the last line of a connection.
constexpr long long CLOSE_GRACE_MS = 200;
// A Ragtime that hasn't written a command line this long after connecting
// gets the connection played anyway.
constexpr long long FIRST_LINE_WAIT_MS = 5000;

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

ReplayHost::ReplayHost(const std::vector<FlightLine>& recording, Options options, int kissPort, int commandPort)
    : speed_(options.speed > 0 ? options.speed : 1.0)
{
    std::vector<FlightLine> lines = recording;
    std::stable_sort(lines.begin(), lines.end(), [](const FlightLine& a, const FlightLine& b) { return a.ms < b.ms; });
    bool open = false;
    for (const FlightLine& line : lines)
    {
        switch (line.kind)
        {
            case Kind::Up:
                segments_.push_back(Segment());
                segments_.back().startMs = segments_.back().endMs = line.ms;
                open = true;
                break;
            case Kind::Down:
                if (!open) break;
                segments_.back().endMs = line.ms;
                segments_.back().closes = true;
                open = false;
                break;
            case Kind::CommandOut:
                if (open && segments_.back().firstOutMs == 0) segments_.back().firstOutMs = line.ms;
                break;
            case Kind::CommandIn:
            case Kind::KissIn:
            case Kind::DataIn:
                // A recording that begins mid-connection (a file rolled
                // over at midnight) plays from its first line.
                if (!open)
                {
                    segments_.push_back(Segment());
                    segments_.back().startMs = line.ms;
                    open = true;
                }
                segments_.back().lines.push_back(line);
                segments_.back().endMs = line.ms;
                break;
            default:
                break;
        }
    }
    if (options.fromMs > 0)
    {
        std::vector<Segment> kept;
        for (Segment& s : segments_)
        {
            if (s.endMs < options.fromMs) continue;
            if (s.startMs < options.fromMs)
            {
                s.startMs = options.fromMs;
                s.lines.erase(std::remove_if(s.lines.begin(), s.lines.end(),
                                             [&](const FlightLine& l) { return l.ms < options.fromMs; }),
                              s.lines.end());
                // Ragtime's opening is long gone: start at the connection.
                s.firstOutMs = 0;
            }
            kept.push_back(std::move(s));
        }
        segments_ = std::move(kept);
    }

    kissListener_ = listenOn(kissPort);
    int commandBound = 0;
    if (kissListener_ >= 0 && commandPort != 0)
    {
        commandListener_ = listenOn(commandPort);
        dataListener_ = commandListener_ >= 0 ? listenOn(commandPort + 1) : -1;
        if (dataListener_ >= 0) commandBound = commandPort;
    }
    // Any free pair: a free command port's + 1 may be taken, so try a few,
    // as FakeHost does.
    std::random_device random;
    for (int attempt = 0; kissListener_ >= 0 && commandPort == 0 && attempt < 100 && commandBound == 0; attempt++)
    {
        int port = 20000 + (int)(random() % 40000);
        commandListener_ = listenOn(port);
        if (commandListener_ < 0) continue;
        dataListener_ = listenOn(port + 1);
        if (dataListener_ < 0)
        {
            closeFd(commandListener_);
            continue;
        }
        commandBound = port;
    }
    if (commandBound == 0)
    {
        closeFd(dataListener_);
        closeFd(commandListener_);
        closeFd(kissListener_);
        throw std::runtime_error("replay host: KISS, command or data port taken");
    }
    kissPort_ = boundPort(kissListener_);
    commandPort_ = commandBound;
}

ReplayHost::~ReplayHost()
{
    closeClients();
    closeFd(kissListener_);
    closeFd(commandListener_);
    closeFd(dataListener_);
}

void ReplayHost::log(uint64_t recordedMs, const std::string& what)
{
    if (!onLog) return;
    std::string when = recordedMs == 0 ? std::string("            ") : ragtime::link::flightTime(recordedMs).substr(11, 12);
    onLog(when + " " + what);
}

uint64_t ReplayHost::recordedNow() const
{
    if (!playing_ || current_ >= segments_.size()) return 0;
    if (waitingSinceMs_ >= 0) return segments_[current_].firstOutMs;
    return segments_[current_].startMs + (uint64_t)((double)(nowMs() - startedMs_) * speed_);
}

void ReplayHost::send(int fd, const std::string& bytes)
{
    const char* at = bytes.data();
    size_t length = bytes.size();
    while (fd >= 0 && length > 0)
    {
        ssize_t n = ::send(fd, at, length, MSG_NOSIGNAL);
        if (n <= 0) return;
        at += n;
        length -= (size_t)n;
    }
}

void ReplayHost::closeClients()
{
    closeFd(kiss_);
    closeFd(command_);
    closeFd(data_);
    commandIn_.clear();
    kissIn_.clear();
}

void ReplayHost::accept()
{
    int fresh = acceptFrom(kissListener_);
    if (fresh >= 0)
    {
        closeFd(kiss_);
        kiss_ = fresh;
        kissIn_.clear();
    }
    fresh = acceptFrom(dataListener_);
    if (fresh >= 0)
    {
        closeFd(data_);
        data_ = fresh;
    }
    fresh = acceptFrom(commandListener_);
    if (fresh >= 0)
    {
        closeFd(command_);
        command_ = fresh;
        commandIn_.clear();
        if (current_ >= segments_.size())
        {
            log(0, "Ragtime connected, but the recording has played to its end");
            return;
        }
        if (playing_) log(recordedNow(), "Ragtime connected again: this connection plays from its start");
        playing_ = true;
        next_ = 0;
        closeAtMs_ = -1;
        const Segment& s = segments_[current_];
        startedMs_ = nowMs();
        waitingSinceMs_ = s.firstOutMs != 0 ? nowMs() : -1;
        log(s.startMs, "Ragtime connected: playing connection " + std::to_string(current_ + 1) + " of "
                           + std::to_string(segments_.size()) + ", recorded "
                           + ragtime::link::flightTime(s.startMs) + " to " + ragtime::link::flightTime(s.endMs));
    }
}

void ReplayHost::readClients()
{
    if (command_ >= 0)
    {
        std::vector<uint8_t> bytes;
        bool open = readFrom(command_, bytes);
        for (uint8_t b : bytes)
        {
            if (b == '\r' || b == '\n')
            {
                if (!commandIn_.empty())
                {
                    if (waitingSinceMs_ >= 0) startClock(segments_[current_].firstOutMs);
                    heard_.push_back(commandIn_);
                    log(recordedNow(), "ragtime> " + ragtime::link::escapeCommandLine(commandIn_));
                }
                commandIn_.clear();
            }
            else
            {
                commandIn_.push_back((char)b);
            }
        }
        if (!open)
        {
            log(recordedNow(), "Ragtime closed the command port");
            if (playing_) log(0, "this connection plays again from its start when Ragtime connects");
            playing_ = false;
            waitingSinceMs_ = -1;
            closeClients();
        }
    }
    if (kiss_ >= 0)
    {
        std::vector<uint8_t> bytes;
        bool open = readFrom(kiss_, bytes);
        for (uint8_t b : bytes)
        {
            if (b != 0xC0)
            {
                kissIn_.push_back(b);
                continue;
            }
            if (!kissIn_.empty())
                log(recordedNow(), "ragtime> KISS frame, " + std::to_string(kissIn_.size()) + " bytes");
            kissIn_.clear();
        }
        if (!open) closeFd(kiss_);
    }
    if (data_ >= 0)
    {
        std::vector<uint8_t> bytes;
        bool open = readFrom(data_, bytes);
        if (!bytes.empty()) log(recordedNow(), "ragtime> " + std::to_string(bytes.size()) + " session bytes");
        if (!open) closeFd(data_);
    }
}

void ReplayHost::startClock(uint64_t recordedMs)
{
    const Segment& s = segments_[current_];
    startedMs_ = nowMs() - (long long)((double)(recordedMs - std::min(recordedMs, s.startMs)) / speed_);
    waitingSinceMs_ = -1;
}

void ReplayHost::play()
{
    if (!playing_ || current_ >= segments_.size()) return;
    if (waitingSinceMs_ >= 0)
    {
        if (nowMs() - waitingSinceMs_ < FIRST_LINE_WAIT_MS) return;
        log(segments_[current_].startMs, "no command line from Ragtime in 5 s: playing anyway");
        startClock(segments_[current_].startMs);
    }
    const Segment& s = segments_[current_];
    auto due = [&](uint64_t ms) {
        return (double)(nowMs() - startedMs_) * speed_ >= (double)(ms - std::min(ms, s.startMs));
    };
    while (next_ < s.lines.size() && due(s.lines[next_].ms))
    {
        const FlightLine& line = s.lines[next_++];
        switch (line.kind)
        {
            case Kind::CommandIn:
                log(line.ms, "host< " + ragtime::link::escapeCommandLine(line.text));
                send(command_, line.text + "\r");
                break;
            case Kind::KissIn:
                if (kiss_ < 0)
                {
                    log(line.ms, "host< KISS frame, " + std::to_string(line.bytes.size())
                                     + " bytes: not played, Ragtime's KISS port isn't connected");
                    break;
                }
                log(line.ms, "host< KISS frame, " + std::to_string(line.bytes.size()) + " bytes");
                send(kiss_, std::string("\xC0") + std::string(line.bytes.begin(), line.bytes.end()) + "\xC0");
                break;
            case Kind::DataIn:
                log(line.ms, "host< " + std::to_string(line.count) + " session bytes (counted, not recorded)");
                break;
            default:
                break;
        }
    }
    if (next_ < s.lines.size() || !due(s.endMs)) return;
    // A moment for the Ragtime to read the last lines before the ports
    // close, as it had them on the air.
    if (closeAtMs_ < 0) closeAtMs_ = nowMs() + CLOSE_GRACE_MS;
    if (nowMs() < closeAtMs_) return;
    closeAtMs_ = -1;

    played_++;
    current_++;
    playing_ = false;
    waitingSinceMs_ = -1;
    if (s.closes)
    {
        log(s.endMs, "the recorded connection closed here: closing the ports");
        closeClients();
    }
    else if (current_ < segments_.size())
    {
        // Ragtime stopped without a "down" (closed or crashed): the next
        // connection is a fresh one.
        log(s.endMs, "the recording breaks off here: closing the ports");
        closeClients();
    }
    else
    {
        log(s.endMs, "end of the recording: the ports stay open");
    }
}

void ReplayHost::step(int timeoutMs)
{
    // Wake in time for the next line due.
    if (playing_ && current_ < segments_.size() && waitingSinceMs_ < 0)
    {
        const Segment& s = segments_[current_];
        uint64_t at = next_ < s.lines.size() ? s.lines[next_].ms : s.endMs;
        long long dueIn = startedMs_ + (long long)((double)(at - std::min(at, s.startMs)) / speed_) - nowMs();
        if (closeAtMs_ >= 0) dueIn = closeAtMs_ - nowMs();
        timeoutMs = (int)std::max(0LL, std::min<long long>(timeoutMs, dueIn));
    }
    std::vector<pollfd> fds;
    for (int fd : {kissListener_, commandListener_, dataListener_, kiss_, command_, data_})
    {
        if (fd >= 0) fds.push_back({fd, POLLIN, 0});
    }
    ::poll(fds.data(), fds.size(), timeoutMs);
    accept();
    readClients();
    play();
}

bool loadRecording(const std::vector<std::string>& paths, std::vector<FlightLine>& out, std::string& why)
{
    out.clear();
    for (const std::string& path : paths)
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
        {
            for (const auto& entry : std::filesystem::directory_iterator(path, ec))
            {
                std::string name = entry.path().filename().string();
                if (name.rfind("flight-", 0) == 0 && entry.path().extension() == ".log") files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            if (files.empty())
            {
                why = "no flight-*.log in " + path;
                return false;
            }
        }
        else
        {
            files.push_back(path);
        }
        for (const auto& file : files)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                why = "can't read " + file.string();
                return false;
            }
            std::ostringstream text;
            text << in.rdbuf();
            std::vector<FlightLine> lines = ragtime::link::parseFlight(text.str());
            out.insert(out.end(), lines.begin(), lines.end());
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const FlightLine& a, const FlightLine& b) { return a.ms < b.ms; });
    if (out.empty())
    {
        why = "no recorded lines";
        return false;
    }
    return true;
}

} // namespace ragtime_test
