// ragtime_fake_air: a bench for trying Ragtime without Data2G. It runs
// fake data2g-hosts on one shared air (tests/FakeHost.h) and a few
// stations of its own that check in, chat and answer, so the station list,
// Globus and the Broadcast tab have something to show. The bots that take
// sessions answer a Call and ragchew: they answer what you type, and
// leave after you send 73.
//
//   ragtime_fake_air [--bots N] [--airtime MS] [--call CALL] [--fade]
//
// --call CALL has the first bot that takes sessions call CALL half a
// minute in, for an incoming session. --fade puts the bots' sessions into
// a 45 s fade every two minutes, so Link quiet shows. A call from host A
// or B is answered on its second try, so "try 2 of 5" shows.
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
#include "ragtime/session/SessionChat.h"

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
    const char* name;
};

const Profile PROFILES[] = {
    {"W1AW", "FN31pr", true, {"QST de W1AW: D2G net bulletin at the top of the hour", "CQ CQ de W1AW", "QRV, 73"},
     "Hiram"},
    {"KC2G", "FN20", true, {"Testing the new polar modes tonight", "CQ D2G de KC2G", "Band is open to EU"}, "Andrew"},
    {"G4XYZ", "IO91wm", false, {"Greetings from London, 73", "CQ de G4XYZ", "Anyone hear me on 20 m?"}, "Nigel"},
    {"VK2ABC", "QF56", true, {"Good morning from Sydney", "CQ CQ de VK2ABC", "Greyline path is up"}, "Bruce"},
    {"JA1RT", "PM95", true, {"CQ de JA1RT", "QRV for a ragchew", "73 from Tokyo"}, "Taro"},
    {"N0CALL", "", true, {"CQ de N0CALL", "Just listening", "73"}, "Pat"},
};

// What a bot says in a ragchew, in turn.
const char* const RAGCHEW[] = {
    "Copy that, fine business.",
    "Conditions are up and down here, some QSB on your signal.",
    "Rig here is 50 watts into a dipole at 10 m.",
    "WX here is clear and cool tonight.",
    "Solid copy now. Anything else for me?",
};

bool says73(const std::string& text)
{
    return text.find("73") != std::string::npos;
}

// A station of the bench's own: a HostLink and a BroadcastChat on its own
// fake host, over real sockets.
struct Bot
{
    Bot(FakeAir& air, const Profile& p, int airtimeMs, uint64_t startMs, std::mt19937& random)
        : host(air), link({p.call, p.sessions, false}), chat(link), session(link), profile(p), random(random)
    {
        host.setAirtimeMs(airtimeMs);
        host.setIdleBroadcastMs(3000);
        kiss = connectTo(host.kissPort());
        command = connectTo(host.commandPort());
        data = connectTo(host.dataPort());
        broadcast::BroadcastChat::Settings s;
        s.callsign = p.call;
        s.locator = p.locator;
        s.acceptsSessions = p.sessions;
        s.software = "fake air bot";
        chat.setSettings(s, 0);
        session::SessionChat::Settings ss;
        ss.callsign = p.call;
        ss.locator = p.locator;
        ss.acceptsSessions = p.sessions;
        ss.software = "fake air bot";
        session.setSettings(ss);
        session.dataPort(data >= 0, startMs);
        link.commandUp(startMs);
        nextMs = startMs;
    }
    ~Bot()
    {
        for (int fd : {kiss, command, data})
        {
            if (fd >= 0) ::close(fd);
        }
    }

    // A ragchew: answer what the far end says, and leave after its 73.
    void inSession(uint64_t now)
    {
        for (const session::SessionChat::Event& e : session.takeEvents())
        {
            using Type = session::SessionChat::Event::Type;
            if (e.type == Type::Started)
            {
                chat.announce(protocol::Status::State::Qrl, e.peer, "", now);
                std::string greeting = std::string("Hello ") + e.peer + ", " + (e.incoming ? "thanks for the call. " : "")
                                       + "Name here is " + profile.name + ", QTH "
                                       + (profile.locator[0] != 0 ? profile.locator : "unknown") + ". How copy?";
                say(greeting, now + 3000);
                fadeAtMs = now + 60000;
            }
            else if (e.type == Type::Heard)
            {
                const session::Message* m = session.find(e.id);
                if (m == nullptr) continue;
                if (says73(m->text))
                {
                    say(std::string("73 ") + e.peer + ", thanks for the ragchew. QRT for now.", now + 4000);
                    leaveMs = now + 9000;
                }
                else
                {
                    say(RAGCHEW[chewed++ % (sizeof(RAGCHEW) / sizeof(RAGCHEW[0]))], now + 4000);
                }
            }
            else if (e.type == Type::Ended)
            {
                chat.announce(protocol::Status::State::Qrv, "", "", now);
                sayText.clear();
                leaveMs = 0;
                fadeAtMs = 0;
                host.fadeSession(false);
            }
        }
        if (!sayText.empty() && now >= sayMs && session.phase() == session::SessionChat::Phase::Connected)
        {
            session.send(sayText, now);
            sayText.clear();
        }
        if (leaveMs != 0 && now >= leaveMs)
        {
            session.leave(now);
            leaveMs = 0;
        }
        // --fade: 45 s of nothing crossing, every two minutes.
        if (fades && fadeAtMs != 0 && now >= fadeAtMs)
        {
            bool fading = (now - fadeAtMs) % 120000 < 45000;
            host.fadeSession(fading);
        }
        if (!callWho.empty() && now >= callMs && link.groupOpen() && session.dataUp())
        {
            if (session.call(callWho, now)) say("Hi " + callWho + ", just trying out a ragchew. How copy?", now + 5000);
            callWho.clear();
        }
    }

    void say(const std::string& text, uint64_t atMs)
    {
        sayText = text;
        sayMs = atMs;
    }

    void pump(uint64_t now)
    {
        std::vector<uint8_t> in;
        readSome(command, in);
        if (!in.empty()) link.commandBytes((const char*)in.data(), in.size(), now);
        in.clear();
        readSome(kiss, in);
        if (!in.empty()) link.kissBytes(in.data(), in.size(), now);
        in.clear();
        readSome(data, in);
        if (!in.empty()) session.dataBytes(in.data(), in.size(), now);
        link.tick(now);
        session.tick(now);

        for (const link::HostLink::Event& e : link.takeEvents())
        {
            session.onEvent(e, now);
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
        inSession(now);

        std::vector<uint8_t> stream = session.takeDataOutput(now);
        sendAll(data, stream.data(), stream.size());
        std::string lines = link.takeCommandOutput();
        sendAll(command, lines.data(), lines.size());
        std::vector<uint8_t> frames = link.takeKissOutput();
        sendAll(kiss, frames.data(), frames.size());
    }

    FakeHost host;
    link::HostLink link;
    broadcast::BroadcastChat chat;
    session::SessionChat session;
    const Profile& profile;
    std::mt19937& random;
    int kiss = -1, command = -1, data = -1;
    std::string sayText;
    uint64_t sayMs = 0;
    uint64_t leaveMs = 0;
    size_t chewed = 0;
    bool fades = false;
    uint64_t fadeAtMs = 0;
    std::string callWho;
    uint64_t callMs = 0;
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
    std::string callWho;
    bool fades = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            bots = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--airtime") == 0 && i + 1 < argc)
            airtimeMs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--call") == 0 && i + 1 < argc)
            callWho = argv[++i];
        else if (std::strcmp(argv[i], "--fade") == 0)
            fades = true;
        else
        {
            std::fprintf(stderr, "usage: ragtime_fake_air [--bots N] [--airtime MS] [--call CALL] [--fade]\n");
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
    for (FakeHost* h : {a.get(), b.get()})
    {
        h->setAirtimeMs(airtimeMs);
        h->setAnswerOnCall(2);
        h->setIdleBroadcastMs(3000);
    }

    std::mt19937 random(12345);
    std::vector<std::unique_ptr<Bot>> stations;
    uint64_t start = nowMs();
    for (int i = 0; i < bots; i++)
    {
        stations.push_back(std::make_unique<Bot>(air, PROFILES[i], airtimeMs, start + 3000 + 2500 * (uint64_t)i,
                                                 random));
        stations.back()->fades = fades;
    }
    for (auto& s : stations)
    {
        if (callWho.empty() || !s->profile.sessions) continue;
        s->callWho = callWho;
        s->callMs = start + 30000;
        break;
    }

    std::printf("Fake data2g-hosts on 127.0.0.1: A KISS 8100, commands 8300, session data 8301;\n"
                "B KISS 8110, commands 8310, session data 8311.\n"
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
