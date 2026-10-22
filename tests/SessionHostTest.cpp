// Two stations in a ragchew, each as the app wires it (HostLink, the
// Broadcast tab's chat and the Session tab's SessionChat) on real sockets
// to its own fake data2g-host, with a third station listening on the
// group: the call answered on the second try, Hellos both ways, messages
// delivered by the modem's acknowledgement, QRL and QRV heard on the
// group, a fade that shows as Link quiet and lifts, and Leave, which the
// far end sees as its partner leaving, not as a lost link.
#include <cstdint>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Check.h"
#include "FakeHost.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/Stations.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/session/SessionChat.h"

using namespace ragtime;
using namespace ragtime_test;
using session::SessionChat;

namespace
{

uint64_t now = 1792627200000; // moves 10 ms a step

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

// A Ragtime as MainFrame wires one, without the window.
struct Station
{
    Station(const std::string& call, FakeHost& host) : link({call, true, false}), chat(link), session(link)
    {
        broadcast::BroadcastChat::Settings b;
        b.callsign = call;
        chat.setSettings(b, 0);
        SessionChat::Settings s;
        s.callsign = call;
        s.locator = "FN31";
        session.setSettings(s);
        stations.setMyCallsign(call);
        kiss = connectTo(host.kissPort());
        command = connectTo(host.commandPort());
        data = connectTo(host.dataPort());
        CHECK(kiss >= 0 && command >= 0 && data >= 0);
        session.dataPort(data >= 0, now);
        link.commandUp(now);
    }
    ~Station()
    {
        for (int fd : {kiss, command, data})
        {
            if (fd >= 0) ::close(fd);
        }
    }

    void pump()
    {
        std::vector<uint8_t> in;
        readSome(command, in);
        if (!in.empty()) link.commandBytes((const char*)in.data(), in.size(), now);
        in.clear();
        readSome(kiss, in);
        if (!in.empty()) link.kissBytes(in.data(), in.size(), now);
        in.clear();
        readSome(data, in);
        if (!in.empty())
        {
            for (const protocol::Record& r : session.dataBytes(in.data(), in.size(), now)) stations.onRecord(r, now);
        }
        link.tick(now);
        session.tick(now);

        for (int round = 0; round < 4; round++)
        {
            for (const link::HostLink::Event& e : link.takeEvents())
            {
                session.onEvent(e, now);
                for (const protocol::Record& r : chat.onEvent(e, now)) stations.onRecord(r, now);
            }
            // What MainFrame does with the session's own events.
            for (const SessionChat::Event& e : session.takeEvents())
            {
                ended = ended || e.type == SessionChat::Event::Type::Ended;
                if (e.type == SessionChat::Event::Type::Calling) stations.setPartner(e.peer, false, now);
                if (e.type == SessionChat::Event::Type::Started)
                {
                    stations.setPartner(e.peer, true, now);
                    chat.announce(protocol::Status::State::Qrl, e.peer, "", now);
                }
                if (e.type == SessionChat::Event::Type::Ended)
                {
                    stations.setPartner("", false, now);
                    chat.announce(protocol::Status::State::Qrv, "", "", now);
                }
            }
        }
        std::vector<uint8_t> out = session.takeDataOutput(now);
        sendAll(data, out.data(), out.size());
        std::string lines = link.takeCommandOutput();
        sendAll(command, lines.data(), lines.size());
        std::vector<uint8_t> frames = link.takeKissOutput();
        sendAll(kiss, frames.data(), frames.size());
    }

    std::string status(const std::string& call) const
    {
        const broadcast::Station* s = stations.find(call);
        return s == nullptr ? std::string() : stations.status(*s, now).word;
    }

    link::HostLink link;
    broadcast::BroadcastChat chat;
    SessionChat session;
    broadcast::Stations stations;
    int kiss = -1, command = -1, data = -1;
    bool ended = false;
};

template <typename Done>
bool run(FakeAir& air, std::vector<Station*> stations, Done done, int steps = 6000)
{
    for (int i = 0; i < steps; i++)
    {
        if (done()) return true;
        air.step(1);
        for (Station* s : stations) s->pump();
        now += 10;
    }
    return done();
}

void testARagchew()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air), hostC(air);
    hostA.setAnswerOnCall(2);
    for (FakeHost* h : {&hostA, &hostB, &hostC}) h->setIdleBroadcastMs(1);
    Station a("AG7EW", hostA), b("W1AW", hostB), c("KC2G", hostC);
    std::vector<Station*> all = {&a, &b, &c};
    CHECK(run(air, all, [&] { return a.link.groupOpen() && b.link.groupOpen() && c.link.groupOpen(); }));

    // AG7EW calls W1AW, who answers the second call.
    CHECK(a.session.call("W1AW", now));
    CHECK(run(air, all, [&] {
        return a.session.phase() == SessionChat::Phase::Connected && b.session.phase() == SessionChat::Phase::Connected;
    }));
    CHECK_EQ(a.session.callTry(), 2);
    CHECK(b.session.incoming() && b.session.peer() == "AG7EW");
    // The caller is on W1AW's list at once, pinned and in session.
    CHECK_EQ(b.status("AG7EW"), std::string("In session with you"));
    CHECK(run(air, all, [&] { return a.session.greeted() && b.session.greeted(); }));

    // Messages both ways, each delivered by the far end's modem.
    uint64_t hello = a.session.send("Good evening, how copy?", now);
    CHECK(run(air, all, [&] { return a.session.find(hello)->state == session::Message::State::Delivered; }));
    CHECK(!b.session.messages().empty() && b.session.messages().back().text == "Good evening, how copy?");
    uint64_t reply = b.session.send("Solid copy here, 73", now);
    CHECK(run(air, all, [&] { return b.session.find(reply)->state == session::Message::State::Delivered; }));
    CHECK(a.session.messages().back().from == "W1AW");

    // KC2G hears both QRLs on the group, sent while the session is idle.
    CHECK(run(air, all, [&] { return c.status("AG7EW") == "QRL with W1AW" && c.status("W1AW") == "QRL with AG7EW"; }));

    // A deep fade: our message waits, and the strip says Link quiet.
    hostA.fadeSession(true);
    uint64_t faded = a.session.send("Are you still there?", now);
    CHECK(run(air, all, [&] { return a.session.quiet(now); }));
    CHECK(a.session.strip(now).amber);
    CHECK(a.session.phase() == SessionChat::Phase::Connected);
    hostA.fadeSession(false);
    CHECK(run(air, all, [&] { return a.session.find(faded)->state == session::Message::State::Delivered; }));
    CHECK(!a.session.quiet(now));

    // AG7EW leaves: W1AW sees it as AG7EW leaving.
    CHECK(a.session.leave(now));
    CHECK(run(air, all, [&] { return a.ended && b.ended; }));
    CHECK(a.session.ending() == SessionChat::Ending::Left);
    CHECK(b.session.ending() == SessionChat::Ending::LeftThere);
    CHECK(b.session.strip(now).text.find("Session ended by AG7EW") == 0);
    CHECK(b.status("AG7EW") != "In session with you");

    // And KC2G hears both QRVs.
    CHECK(run(air, all, [&] { return c.status("AG7EW") == "QRV" && c.status("W1AW") == "QRV"; }));
}

void testNoAnswer()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    Station a("AG7EW", hostA), b("W1AW", hostB);
    std::vector<Station*> all = {&a, &b};
    CHECK(run(air, all, [&] { return a.link.groupOpen() && b.link.groupOpen(); }));
    CHECK(a.session.call("K1ABC", now));
    CHECK(run(air, all, [&] { return a.ended; }));
    CHECK(a.session.ending() == SessionChat::Ending::NoAnswer);
    CHECK_EQ(a.session.callTry(), 5);
    // No QRL went, so no QRV either.
    CHECK_EQ(hostA.burstsSent(), (size_t)0);
}

} // namespace

int main()
{
    testARagchew();
    testNoAnswer();
    return ragtime_test::finish();
}
