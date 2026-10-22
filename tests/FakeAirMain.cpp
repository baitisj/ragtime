// ragtime_fake_air: a bench for trying Ragtime without Data2G. It runs
// fake data2g-hosts on one shared air (tests/FakeHost.h) and a few
// stations of its own that check in, chat and answer, so the station list,
// Globus and the Broadcast tab have something to show.
//
//   ragtime_fake_air [--bots N] [--airtime MS]
//
// Host A listens on 127.0.0.1:8100 (KISS) and 8300 (commands), host B on
// 8110 and 8310, so two Ragtimes (each with its own --config file) can talk
// to each other through it. Nothing here is Data2G's, and nothing goes on
// the radio. Ctrl-C stops it.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "FakeHost.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"

using namespace ragtime;
using namespace ragtime_test;

namespace
{

volatile std::sig_atomic_t stopping = 0;

uint64_t nowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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

void readSome(int fd, std::vector<uint8_t>& out)
{
    uint8_t buffer[4096];
    for (;;)
    {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n <= 0) return;
        out.insert(out.end(), buffer, buffer + n);
    }
}

struct Profile
{
    const char* call;
    const char* locator;
    bool sessions;
    const char* lines[3];
};

const Profile PROFILES[] = {
    {"W1AW", "FN31pr", true, {"QST de W1AW: D2G net bulletin at the top of the hour", "CQ CQ de W1AW", "QRV, 73"}},
    {"KC2G", "FN20", true, {"Testing the new polar modes tonight", "CQ D2G de KC2G", "Band is open to EU"}},
    {"G4XYZ", "IO91wm", false, {"Greetings from London, 73", "CQ de G4XYZ", "Anyone hear me on 20 m?"}},
    {"VK2ABC", "QF56", true, {"Good morning from Sydney", "CQ CQ de VK2ABC", "Greyline path is up"}},
    {"JA1RT", "PM95", true, {"CQ de JA1RT", "QRV for a ragchew", "73 from Tokyo"}},
    {"N0CALL", "", true, {"CQ de N0CALL", "Just listening", "73"}},
};

// A station of the bench's own: a HostLink and a BroadcastChat on its own
// fake host, over real sockets.
struct Bot
{
    Bot(FakeAir& air, const Profile& p, int airtimeMs, uint64_t startMs, std::mt19937& random)
        : host(air), link({p.call, p.sessions, false}), chat(link), profile(p), random(random)
    {
        host.setAirtimeMs(airtimeMs);
        kiss = connectTo(host.kissPort());
        command = connectTo(host.commandPort());
        broadcast::BroadcastChat::Settings s;
        s.callsign = p.call;
        s.locator = p.locator;
        s.acceptsSessions = p.sessions;
        s.software = "fake air bot";
        chat.setSettings(s, 0);
        link.commandUp(startMs);
        nextMs = startMs;
    }
    ~Bot()
    {
        if (kiss >= 0) ::close(kiss);
        if (command >= 0) ::close(command);
    }

    void pump(uint64_t now)
    {
        std::vector<uint8_t> in;
        readSome(command, in);
        if (!in.empty()) link.commandBytes((const char*)in.data(), in.size(), now);
        in.clear();
        readSome(kiss, in);
        if (!in.empty()) link.kissBytes(in.data(), in.size(), now);
        link.tick(now);

        for (const link::HostLink::Event& e : link.takeEvents())
        {
            for (const protocol::Record& r : chat.onEvent(e, now))
            {
                const protocol::Text* text = r.as<protocol::Text>();
                if (text != nullptr && text->to == profile.call)
                {
                    replyTo = r.header.callsign;
                    replyMs = now + 4000;
                }
            }
        }

        if (link.groupOpen() && now >= nextMs)
        {
            // A check-in now and then, as an operator would, so a Ragtime
            // started later still learns the locators.
            if (now >= checkInMs)
            {
                chat.checkIn("", now);
                checkInMs = now + 180000;
            }
            else
            {
                chat.send(profile.lines[line++ % 3], "", now);
            }
            std::uniform_int_distribution<int> wait(20000, 90000);
            nextMs = now + (uint64_t)wait(random);
        }
        if (!replyTo.empty() && now >= replyMs)
        {
            chat.send("@" + replyTo + " copy you fine, thanks. 73 de " + profile.call, "", now);
            replyTo.clear();
        }

        std::string lines = link.takeCommandOutput();
        sendAll(command, lines.data(), lines.size());
        std::vector<uint8_t> frames = link.takeKissOutput();
        sendAll(kiss, frames.data(), frames.size());
    }

    FakeHost host;
    link::HostLink link;
    broadcast::BroadcastChat chat;
    const Profile& profile;
    std::mt19937& random;
    int kiss = -1, command = -1;
    uint64_t nextMs = 0;
    uint64_t checkInMs = 0;
    size_t line = 0;
    std::string replyTo;
    uint64_t replyMs = 0;
};

} // namespace

int main(int argc, char** argv)
{
    int bots = 4;
    int airtimeMs = 1500;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            bots = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--airtime") == 0 && i + 1 < argc)
            airtimeMs = std::atoi(argv[++i]);
        else
        {
            std::fprintf(stderr, "usage: ragtime_fake_air [--bots N] [--airtime MS]\n");
            return 2;
        }
    }
    const int maxBots = (int)(sizeof(PROFILES) / sizeof(PROFILES[0]));
    if (bots < 0) bots = 0;
    if (bots > maxBots) bots = maxBots;

    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });

    FakeAir air;
    std::unique_ptr<FakeHost> a, b;
    try
    {
        a = std::make_unique<FakeHost>(air, 2, 8100, 8300);
        b = std::make_unique<FakeHost>(air, 2, 8110, 8310);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "ragtime_fake_air: %s (is another bench or data2g-host running?)\n", e.what());
        return 1;
    }
    a->setAirtimeMs(airtimeMs);
    b->setAirtimeMs(airtimeMs);

    std::mt19937 random(12345);
    std::vector<std::unique_ptr<Bot>> stations;
    uint64_t start = nowMs();
    for (int i = 0; i < bots; i++)
        stations.push_back(std::make_unique<Bot>(air, PROFILES[i], airtimeMs, start + 3000 + 2500 * (uint64_t)i,
                                                 random));

    std::printf("Fake data2g-hosts on 127.0.0.1: A KISS 8100, commands 8300; B KISS 8110, commands 8310.\n"
                "%d bot station%s on the air. Ctrl-C to stop.\n",
                bots, bots == 1 ? "" : "s");
    std::fflush(stdout);
    while (!stopping)
    {
        air.step(20);
        uint64_t now = nowMs();
        for (auto& s : stations) s->pump(now);
    }
    return 0;
}
