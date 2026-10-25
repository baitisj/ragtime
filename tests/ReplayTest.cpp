// A flight recording made as HostConnection makes one (a HostLink on real
// sockets to a fake data2g-host, hearing another station's broadcast),
// then played back by ReplayHost to a fresh HostLink, which must hear the
// same: the group opened, the modes listed, the frame and who sent it.
// And the recording's connections: cut at "down", started at --from, and
// played from the first line when a file begins mid-connection.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Check.h"
#include "FakeHost.h"
#include "ReplayHost.h"
#include "ragtime/link/FlightRecorder.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using namespace ragtime_test;
using link::FlightLine;
using link::FlightRecorder;
using link::HostLink;
using Type = HostLink::Event::Type;

namespace
{

uint64_t now = 1792945542000; // 2026-10-25T16:25:42Z, in ms; moves 10 ms a step

int connectTo(int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (fd >= 0 && ::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        ::close(fd);
        fd = -1;
    }
    return fd;
}

void sendAll(int fd, const void* bytes, size_t length)
{
    const uint8_t* at = (const uint8_t*)bytes;
    while (fd >= 0 && length > 0)
    {
        ssize_t n = ::send(fd, at, length, MSG_NOSIGNAL);
        if (n <= 0) return;
        at += n;
        length -= (size_t)n;
    }
}

bool readSome(int fd, std::vector<uint8_t>& out)
{
    uint8_t buffer[4096];
    for (;;)
    {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0)
        {
            out.insert(out.end(), buffer, buffer + n);
            continue;
        }
        return n != 0;
    }
}

// A HostLink on the KISS and command ports, tapped for the recorder as
// HostConnection taps it (when one is given).
struct Client
{
    Client(const std::string& call, int kissPort, int commandPort, FlightRecorder* recorder)
        : link({call, true, false}), recorder(recorder)
    {
        kiss = connectTo(kissPort);
        command = connectTo(commandPort);
        CHECK(kiss >= 0 && command >= 0);
        if (recorder) recorder->up("127.0.0.1 KISS " + std::to_string(kissPort) + " commands " + std::to_string(commandPort), now);
        link.commandUp(now);
    }
    ~Client()
    {
        if (kiss >= 0) ::close(kiss);
        if (command >= 0) ::close(command);
    }

    void pump()
    {
        std::vector<uint8_t> in;
        if (command >= 0)
        {
            bool open = readSome(command, in);
            if (!in.empty())
            {
                if (recorder) recorder->commandIn((const char*)in.data(), in.size(), now);
                link.commandBytes((const char*)in.data(), in.size(), now);
            }
            if (!open)
            {
                ::close(command);
                command = -1;
                closed = true;
                link.commandDown(now);
            }
        }
        in.clear();
        if (kiss >= 0) readSome(kiss, in);
        if (!in.empty())
        {
            if (recorder) recorder->kissIn(in.data(), in.size(), now);
            link.kissBytes(in.data(), in.size(), now);
        }
        link.tick(now);
        for (HostLink::Event& e : link.takeEvents()) events.push_back(std::move(e));
        std::string lines = link.takeCommandOutput();
        if (command >= 0 && !lines.empty())
        {
            sendAll(command, lines.data(), lines.size());
            if (recorder) recorder->commandOut(lines.data(), lines.size(), now);
        }
        std::vector<uint8_t> frames = link.takeKissOutput();
        if (!frames.empty())
        {
            sendAll(kiss, frames.data(), frames.size());
            if (recorder) recorder->kissOut(frames.data(), frames.size(), now);
        }
    }

    const HostLink::Event* first(Type type) const
    {
        for (const auto& e : events)
        {
            if (e.type == type) return &e;
        }
        return nullptr;
    }

    HostLink link;
    FlightRecorder* recorder;
    int kiss = -1, command = -1;
    bool closed = false;
    std::vector<HostLink::Event> events;
};

std::vector<uint8_t> textFrame(const std::string& call, uint32_t seq, const std::string& text)
{
    protocol::Text body;
    body.text = text;
    protocol::Record record;
    record.header = {call, seq, (uint32_t)(now / 1000)};
    record.body = body;
    return protocol::encodeFrame({record});
}

bool runAir(FakeAir& air, const std::vector<Client*>& clients, const std::function<bool()>& done)
{
    for (int i = 0; i < 2000; i++)
    {
        air.step(2);
        now += 10;
        for (Client* c : clients) c->pump();
        if (done()) return true;
    }
    return false;
}

bool runReplay(ReplayHost& host, Client& client, const std::function<bool()>& done)
{
    for (int i = 0; i < 10000; i++)
    {
        host.step(2);
        now += 10;
        client.pump();
        if (done()) return true;
    }
    return false;
}

void testRecordAndReplay()
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "ragtime-replay-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    FlightRecorder recorder;
    recorder.setFolder(dir.string(), now);

    std::vector<uint8_t> frame = textFrame("KC2G", 1, "Testing the new polar modes tonight");
    {
        FakeAir air;
        FakeHost hostA(air), hostB(air);
        Client a("AG7EW", hostA.kissPort(), hostA.commandPort(), &recorder);
        Client b("KC2G", hostB.kissPort(), hostB.commandPort(), nullptr);
        CHECK(runAir(air, {&a, &b}, [&] { return a.link.groupOpen() && b.link.groupOpen() && !a.link.modes().empty(); }));
        b.link.broadcast({frame}, "", now);
        CHECK(runAir(air, {&a, &b}, [&] { return a.first(Type::Heard) != nullptr; }));
        recorder.down("closed by Ragtime", now);
    }

    std::vector<FlightLine> recording;
    std::string why;
    CHECK(loadRecording({dir.string()}, recording, why));
    bool sawFrame = false;
    for (const FlightLine& l : recording) sawFrame = sawFrame || (l.kind == FlightLine::Kind::KissIn);
    CHECK(sawFrame);

    // Twice as fast: the recording took a few hundred simulated ms.
    ReplayHost::Options options;
    options.speed = 2.0;
    ReplayHost replay(recording, options);
    std::vector<std::string> log;
    replay.onLog = [&](const std::string& line) { log.push_back(line); };
    CHECK_EQ(replay.connections(), (size_t)1);

    Client z("AG7EW", replay.kissPort(), replay.commandPort(), nullptr);
    CHECK(runReplay(replay, z, [&] { return replay.finished() && z.closed; }));
    CHECK(z.link.groupOpen() || z.first(Type::GroupOpen) != nullptr);
    CHECK_EQ(z.link.modes().size(), link::knownModes().size());
    const HostLink::Event* heard = z.first(Type::Heard);
    CHECK(heard != nullptr && heard->frame == frame);
    const HostLink::Event* from = z.first(Type::HeardFrom);
    CHECK(from != nullptr && from->text == "KC2G");
    // What the Ragtime said was read, not answered.
    bool mycall = false;
    for (const std::string& line : replay.heard()) mycall = mycall || line == "MYCALL AG7EW";
    CHECK(mycall);
    bool closing = false;
    for (const std::string& line : log) closing = closing || line.find("closing the ports") != std::string::npos;
    CHECK(closing);
    std::filesystem::remove_all(dir, ec);
}

FlightLine line(uint64_t ms, FlightLine::Kind kind, const std::string& text = std::string())
{
    FlightLine l;
    l.ms = ms;
    l.kind = kind;
    l.text = text;
    return l;
}

void testConnections()
{
    using K = FlightLine::Kind;
    const uint64_t t = 1792945542000;
    std::vector<FlightLine> recording = {
        // A file that begins mid-connection, broken off without a "down".
        line(t, K::CommandIn, "BUSY ON"),
        line(t + 100, K::CommandOut, "MODES"),
        line(t + 200, K::CommandIn, "BUSY OFF"),
        // Ragtime opened again.
        line(t + 5000, K::Note, "Ragtime 0.1.2 opened"),
        line(t + 6000, K::Up, "127.0.0.1 KISS 8100 commands 8300"),
        line(t + 6100, K::CommandIn, "OK"),
        line(t + 9000, K::CommandIn, "PTT ON"),
        line(t + 9500, K::Down, "data2g-host closed its command port"),
        line(t + 20000, K::Up, "127.0.0.1 KISS 8100 commands 8300"),
        line(t + 20100, K::CommandIn, "OK"),
    };
    {
        ReplayHost replay(recording, ReplayHost::Options());
        CHECK_EQ(replay.connections(), (size_t)3);
    }
    {
        // From the middle of the second: it starts there, without the OK.
        ReplayHost::Options options;
        options.fromMs = t + 7000;
        ReplayHost replay(recording, options);
        CHECK_EQ(replay.connections(), (size_t)2);
        Client z("AG7EW", replay.kissPort(), replay.commandPort(), nullptr);
        std::vector<std::string> log;
        replay.onLog = [&](const std::string& l) { log.push_back(l); };
        // 2.5 s of recording to "down".
        CHECK(runReplay(replay, z, [&] { return replay.played() == 1; }));
        bool ok = false, ptt = false;
        for (const std::string& l : log)
        {
            ok = ok || l.find("host< OK") != std::string::npos;
            ptt = ptt || l.find("host< PTT ON") != std::string::npos;
        }
        CHECK(!ok && ptt);
    }
    {
        ReplayHost::Options options;
        options.fromMs = t + 30000;
        ReplayHost replay(recording, options);
        // Past the end: nothing to play.
        CHECK_EQ(replay.connections(), (size_t)0);
    }
}

} // namespace

int main()
{
    testRecordAndReplay();
    testConnections();
    return ragtime_test::finish();
}
