// Two stations, each a HostLink on real sockets to its own fake
// data2g-host, the hosts sharing one air: a broadcast heard, a session
// called and answered, Hellos, a text and a file through the session, a
// burst held by the session and sent after it, and the host's refusals,
// drops and reports.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Check.h"
#include "FakeHost.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"

using namespace ragtime;
using namespace ragtime_test;
using link::HostLink;
using Type = HostLink::Event::Type;

namespace
{

namespace fs = std::filesystem;

uint64_t now = 1792627200000; // 2026-10-22 00:00 UTC, in ms; moves 10 ms a step

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

// What has come on a socket; false once it has closed.
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

// A station as the app will be: HostLink on the KISS and command ports, its
// own session stream on the data port, and the session file engine.
struct Station
{
    Station(const std::string& call, FakeHost& host) : link({call, true, false}), call(call), host(host)
    {
        kiss = connectTo(host.kissPort());
        command = connectTo(host.commandPort());
        data = connectTo(host.dataPort());
        CHECK(kiss >= 0 && command >= 0 && data >= 0);
        link.commandUp(now);
    }
    ~Station()
    {
        for (int fd : {kiss, command, data})
        {
            if (fd >= 0) ::close(fd);
        }
    }

    // Reads every port, and writes what the link and the session have.
    void pump()
    {
        std::vector<uint8_t> in;
        if (command >= 0)
        {
            bool open = readSome(command, in);
            if (!in.empty()) link.commandBytes((const char*)in.data(), in.size(), now);
            if (!open)
            {
                ::close(command);
                command = -1;
                link.commandDown(now);
            }
        }
        in.clear();
        readSome(kiss, in);
        if (!in.empty()) link.kissBytes(in.data(), in.size(), now);
        link.tick(now);

        in.clear();
        readSome(data, in);
        if (!in.empty())
        {
            link.sessionHeard(now);
            std::vector<protocol::Record> records;
            reader.feed(in, records);
            for (const protocol::Record& r : records)
            {
                session.push_back(r);
                if (r.type() >= (uint8_t)protocol::RecordType::FileOffer && r.type() <= (uint8_t)protocol::RecordType::FileCancel)
                {
                    files.onRecord(reader.peer(), r, now);
                }
            }
        }

        for (HostLink::Event& e : link.takeEvents())
        {
            if (e.type == Type::Buffer) files.acknowledged(link.sessionPeer(), written - (uint64_t)e.count);
            if (e.type == Type::SessionConnected)
            {
                // A fresh numbering for each session, and our Hello first.
                // The far end's Hello can come on the data port before
                // CONNECTED is read on the command port, so the reader is
                // made ready when a session ends, not when one starts.
                numbering = protocol::Numbering(call);
                written = 0;
                write(protocol::streamOpening(numbering.record(protocol::Hello(), (uint32_t)(now / 1000))));
            }
            if (e.type == Type::SessionEnded)
            {
                files.sessionEnded(e.peer, now);
                reader.reset();
            }
            events.push_back(std::move(e));
        }

        if (link.sessionState() == HostLink::SessionState::Connected && reader.greeted())
        {
            const std::string& peer = link.sessionPeer();
            std::vector<uint8_t> out = files.takeRecords(peer, numbering, now);
            files.takePiece(peer, numbering, written + out.size(), now, out);
            write(out);
        }
        files.tick(now);

        std::string lines = link.takeCommandOutput();
        if (command >= 0) sendAll(command, lines.data(), lines.size());
        std::vector<uint8_t> frames = link.takeKissOutput();
        sendAll(kiss, frames.data(), frames.size());
    }

    void write(const std::vector<uint8_t>& bytes)
    {
        sendAll(data, bytes.data(), bytes.size());
        written += bytes.size();
    }

    bool heard(Type type) const
    {
        for (const auto& e : events)
        {
            if (e.type == type) return true;
        }
        return false;
    }

    HostLink link;
    std::string call;
    FakeHost& host;
    int kiss = -1, command = -1, data = -1;
    std::vector<HostLink::Event> events;
    protocol::Numbering numbering;
    protocol::StreamReader reader;
    std::vector<protocol::Record> session;
    files::SessionFiles files;
    uint64_t written = 0;
};

// Steps the air and both stations until done() or a simulated minute.
template <typename Done>
bool run(FakeAir& air, Station& a, Station& b, Done done)
{
    for (int i = 0; i < 6000; i++)
    {
        if (done()) return true;
        air.step(1);
        a.pump();
        b.pump();
        now += 10;
    }
    return done();
}

std::vector<protocol::Record> heardRecords(const Station& s)
{
    std::vector<protocol::Record> records;
    for (const auto& e : s.events)
    {
        if (e.type == Type::Heard) CHECK(protocol::decodeFrame(e.frame, records));
    }
    return records;
}

std::vector<uint8_t> textFrame(const std::string& call, uint32_t seq, const std::string& text)
{
    protocol::Text body;
    body.text = text;
    protocol::Record record;
    record.header = {call, seq, (uint32_t)(now / 1000)};
    record.body = body;
    return protocol::encodeFrame({record});
}

void testTwoStations()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    Station a("AG7EW", hostA), b("W1AW", hostB);

    // The opening: both groups open, the modes listed.
    CHECK(run(air, a, b, [&] { return a.link.groupOpen() && b.link.groupOpen() && !b.link.modes().empty(); }));
    CHECK(hostA.callsign() == "AG7EW" && hostA.listening() && hostA.groupOpen());
    CHECK_EQ(a.link.groupPort(), 2);
    CHECK_EQ(a.link.modes().size(), link::knownModes().size());

    // A broadcast, heard at B with the sender's call.
    uint64_t id = a.link.broadcast({textFrame("AG7EW", 1, "CQ CQ de AG7EW")}, "", now);
    CHECK(run(air, a, b, [&] { return a.link.burstState(id) == HostLink::BurstState::Sent; }));
    // The KISS port and the command port are read separately: wait for both.
    CHECK(run(air, a, b, [&] {
        return b.heard(Type::Heard) && b.heard(Type::HeardFrom) && a.heard(Type::Ptt) && a.heard(Type::ModeUsed);
    }));
    std::vector<protocol::Record> heard = heardRecords(b);
    CHECK_EQ(heard.size(), (size_t)1);
    if (heard.size() == 1)
    {
        CHECK_EQ(heard[0].header.callsign, std::string("AG7EW"));
        CHECK(heard[0].as<protocol::Text>() && heard[0].as<protocol::Text>()->text == "CQ CQ de AG7EW");
    }
    bool from = false;
    for (const auto& e : b.events) from = from || (e.type == Type::HeardFrom && e.text == "AG7EW" && !e.snr);
    CHECK(from);  // a host from before Data2G PR #98: no SNR
    CHECK(a.heard(Type::Ptt) && a.heard(Type::ModeUsed));
    CHECK(heardRecords(a).empty()); // a host doesn't hear itself

    // A calls B; both are connected, B's the incoming end.
    CHECK(a.link.call("W1AW", now));
    CHECK(run(air, a, b, [&] {
        return a.link.sessionState() == HostLink::SessionState::Connected &&
               b.link.sessionState() == HostLink::SessionState::Connected;
    }));
    CHECK(!a.link.sessionIncoming() && b.link.sessionIncoming());
    CHECK_EQ(b.link.sessionPeer(), std::string("AG7EW"));

    // Hellos both ways, then a text.
    CHECK(run(air, a, b, [&] { return a.reader.greeted() && b.reader.greeted(); }));
    CHECK_EQ(a.reader.peer(), std::string("W1AW"));
    CHECK_EQ(b.reader.peer(), std::string("AG7EW"));
    protocol::Text text;
    text.text = "Thanks for the report, 73";
    a.write(protocol::encodeRecord(a.numbering.record(text, (uint32_t)(now / 1000))));
    CHECK(run(air, a, b, [&] { return b.session.size() >= 2; }));
    if (b.session.size() >= 2)
    {
        CHECK(b.session[0].as<protocol::Hello>() != nullptr);
        const auto* got = b.session[1].as<protocol::Text>();
        CHECK(got != nullptr && got->text == "Thanks for the report, 73");
        CHECK_EQ(b.session[1].header.seq, (uint32_t)1);
    }

    // A file, taken without asking.
    fs::path dir = fs::temp_directory_path() / ("ragtime-fake-host-" + std::to_string(std::random_device()()));
    fs::create_directories(dir);
    std::string content(10000, '\0');
    for (size_t i = 0; i < content.size(); i++) content[i] = (char)(i * 13 + 5);
    {
        std::ofstream out(dir / "log.adi", std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
    }
    fs::create_directories(dir / "in");
    b.files.setAutoAccept(files::utf8FromPath(dir / "in"), {"AG7EW"});
    std::string error;
    uint64_t file = a.files.offer("W1AW", files::utf8FromPath(dir / "log.adi"), now, error);
    CHECK(file != 0);
    CHECK(run(air, a, b, [&] {
        for (const auto& t : a.files.transfers())
        {
            if (t.id == file) return t.state == files::FileTransfer::State::Delivered;
        }
        return false;
    }));
    std::ifstream in(dir / "in" / "log.adi", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) == content);

    // The session holds the group: a burst waits for it.
    uint64_t held = a.link.broadcast({textFrame("AG7EW", 3, "QRZ?")}, "", now);
    CHECK(run(air, a, b, [&] { return a.link.burstState(held) == HostLink::BurstState::Held; }));
    for (int i = 0; i < 20; i++)
    {
        air.step(1);
        a.pump();
        b.pump();
    }
    CHECK(a.link.burstState(held) == HostLink::BurstState::Held && heardRecords(b).size() == 1);

    // Leave: both ends hear it, and then the burst goes.
    CHECK(a.link.leave(now));
    CHECK(run(air, a, b, [&] {
        return a.link.sessionState() == HostLink::SessionState::None &&
               b.link.sessionState() == HostLink::SessionState::None;
    }));
    bool left = false, ended = false;
    for (const auto& e : a.events) left = left || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::Left);
    for (const auto& e : b.events) ended = ended || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::Ended);
    CHECK(left && ended);
    CHECK(run(air, a, b, [&] { return a.link.burstState(held) == HostLink::BurstState::Sent; }));
    CHECK(run(air, a, b, [&] { return heardRecords(b).size() == 2; }));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Hosts since Data2G PR #118: DATA2G ON, and CLEANTXBUFFER drops what a
// session's host holds, in host byte offsets.
void testACut()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    hostA.setSessionRate(100);
    Station a("AG7EW", hostA), b("W1AW", hostB);
    CHECK(run(air, a, b, [&] { return a.link.groupOpen() && b.link.groupOpen(); }));
    CHECK(a.link.data2g() && !a.link.canCut());
    CHECK(a.link.call("W1AW", now));
    CHECK(run(air, a, b, [&] { return a.link.canCut() && b.link.canCut(); }));

    protocol::Text text;
    text.text = std::string(1500, 'x');
    a.write(protocol::encodeRecord(a.numbering.record(text, (uint32_t)(now / 1000))));
    CHECK(run(air, a, b, [&] { return hostA.sessionWaiting() > 1000; }));
    uint64_t written = a.written;
    CHECK(a.link.cleanTxBuffer(written, now));
    link::CommandEvent::Clean clean;
    CHECK(run(air, a, b, [&] {
        for (const auto& e : a.events)
        {
            if (e.type == Type::Clean) clean = e.clean;
        }
        return !clean.offsets.empty();
    }));
    CHECK(clean.result == link::CommandEvent::Clean::Result::Ok && clean.offsets.size() == 4);
    if (clean.offsets.size() == 4)
    {
        CHECK(clean.offsets[0] < written && clean.offsets[1] == written);
        CHECK(clean.offsets[2] == clean.offsets[0] && clean.offsets[3] == clean.offsets[0]);
        CHECK_EQ(hostA.cutBytes(), (size_t)(written - clean.offsets[0]));
    }
    CHECK_EQ(hostA.sessionWaiting(), (size_t)0);

    // A host from before it: no cuts.
    FakeAir oldAir;
    FakeHost oldA(oldAir), oldB(oldAir);
    oldA.setCuts(false);
    Station c("AG7EW", oldA), d("W1AW", oldB);
    CHECK(run(oldAir, c, d, [&] { return c.link.groupOpen() && !c.link.modes().empty(); }));
    CHECK(!c.link.data2g() && !c.heard(Type::Refused));
}

// Hosts since Data2G PR #98: an SNR on HEARD, and SN in a session.
void testSnrs()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    hostA.setReportsSnr(true);
    hostB.setReportsSnr(true);
    hostA.setSignalDb(4.0);
    hostB.setSignalDb(-3.0);
    Station a("AG7EW", hostA), b("W1AW", hostB);
    CHECK(run(air, a, b, [&] { return a.link.groupOpen() && b.link.groupOpen(); }));

    a.link.broadcast({textFrame("AG7EW", 1, "CQ CQ de AG7EW")}, "", now);
    auto heardAt = [](const Station& s, Type type, const std::string& call, double dB) {
        for (const auto& e : s.events)
        {
            const std::string& who = type == Type::HeardFrom ? e.text : e.peer;
            if (e.type == type && who == call && e.snr && std::fabs(*e.snr - dB) < 0.05) return true;
        }
        return false;
    };
    CHECK(run(air, a, b, [&] { return heardAt(b, Type::HeardFrom, "AG7EW", 4.0); }));

    // Each end asks SN once the other's burst is in: B on A's Hello, A
    // on B's acknowledging it.
    CHECK(a.link.call("W1AW", now));
    CHECK(run(air, a, b, [&] {
        return heardAt(b, Type::SessionSnr, "AG7EW", 4.0) && heardAt(a, Type::SessionSnr, "W1AW", -3.0);
    }));
    bool refused = false;
    for (const Station* s : {&a, &b})
    {
        for (const auto& e : s->events) refused = refused || e.type == Type::Refused;
    }
    CHECK(!refused);
}

void testTheHostSaysNo()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    Station a("AG7EW", hostA), b("W1AW", hostB);
    CHECK(run(air, a, b, [&] { return a.link.groupOpen() && !a.link.modes().empty(); }));

    // Nobody answers a call to a station that isn't there.
    CHECK(a.link.call("K1ABC", now));
    CHECK(run(air, a, b, [&] { return a.link.sessionState() == HostLink::SessionState::None; }));
    bool noAnswer = false;
    for (const auto& e : a.events) noAnswer = noAnswer || (e.type == Type::SessionEnded && e.sessionEnd == HostLink::SessionEnd::NoAnswer);
    CHECK(noAnswer);

    // A burst the host drops is not sent.
    hostA.dropNextBurst();
    uint64_t dropped = a.link.broadcast({textFrame("AG7EW", 1, "lost")}, "", now);
    CHECK(run(air, a, b, [&] { return a.link.burstState(dropped) == HostLink::BurstState::NotSent; }));
    CHECK(heardRecords(b).empty());

    // A mode the host won't take is refused, and the burst still goes.
    hostA.script("BCAST MODE", {"WRONG"});
    uint64_t refused = a.link.broadcast({textFrame("AG7EW", 2, "slow")}, "fsk16r25-r1/3", now);
    CHECK(run(air, a, b, [&] { return a.heard(Type::Refused); }));
    CHECK(run(air, a, b, [&] {
        HostLink::BurstState s = a.link.burstState(refused);
        return s == HostLink::BurstState::Sent || s == HostLink::BurstState::NotSent;
    }));

    // Reports out of the blue.
    hostB.say("BUSY ON");
    CHECK(run(air, a, b, [&] { return b.link.busy(); }));
    hostB.say("BUSY OFF");
    CHECK(run(air, a, b, [&] { return !b.link.busy(); }));

    // Held bursts wait; let go, they're sent.
    hostA.holdBursts(true);
    uint64_t waiting = a.link.broadcast({textFrame("AG7EW", 3, "wait")}, "", now);
    for (int i = 0; i < 50; i++)
    {
        air.step(1);
        a.pump();
        b.pump();
        now += 10;
    }
    CHECK(a.link.burstState(waiting) == HostLink::BurstState::Sending);
    hostA.holdBursts(false);
    CHECK(run(air, a, b, [&] { return a.link.burstState(waiting) == HostLink::BurstState::Sent; }));

    // The host goes away: the group closes with it.
    hostB.closeCommand();
    CHECK(run(air, a, b, [&] { return !b.link.commandConnected(); }));
    CHECK(b.heard(Type::GroupClosed) && !b.link.groupOpen());
}

} // namespace

// Hosts since Data2G PR #122: DISCONNECTED says why, and TIMEOUT and
// TXPOWER read and set the idle timeout and the output volume.
void testReasonsAndTheIdleTimeout()
{
    FakeAir air;
    FakeHost hostA(air), hostB(air);
    Station a("AG7EW", hostA), b("W1AW", hostB);
    CHECK(run(air, a, b, [&] {
        return a.link.groupOpen() && b.link.groupOpen() && a.link.idleTimeout() && a.link.txPower()
               && b.link.idleTimeout();
    }));
    CHECK(*a.link.idleTimeout() == 300.0 && *a.link.txPower() == 0.0);
    auto ended = [](const Station& s, const std::string& why) {
        for (const auto& e : s.events)
        {
            if (e.type == Type::SessionEnded && e.disconnect.reason == why) return true;
        }
        return false;
    };

    // A station not taking sessions says so at the first call.
    b.link.setAcceptSessions(false, now);
    CHECK(run(air, a, b, [&] { return !hostB.listening(); }));
    CHECK(a.link.call("W1AW", now));
    CHECK(run(air, a, b, [&] { return a.link.sessionState() == HostLink::SessionState::None; }));
    CHECK(ended(a, "NOT_LISTENING"));
    b.link.setAcceptSessions(true, now);
    CHECK(run(air, a, b, [&] { return hostB.listening(); }));

    // A short idle timeout at A: the quiet session is closed, IDLE here and
    // PEER there.
    a.link.setIdleTimeout(0.3, now);
    CHECK(run(air, a, b, [&] { return a.link.idleTimeout() && *a.link.idleTimeout() == 0.3; }));
    CHECK(hostA.idleTimeoutS() == 0.3);
    CHECK(a.link.call("W1AW", now));
    CHECK(run(air, a, b, [&] { return a.link.sessionState() == HostLink::SessionState::Connected; }));
    CHECK(run(air, a, b, [&] {
        return a.link.sessionState() == HostLink::SessionState::None
               && b.link.sessionState() == HostLink::SessionState::None;
    }));
    CHECK(ended(a, "IDLE") && ended(b, "PEER"));

    // The output volume.
    CHECK(a.link.setTxPower(-6.0, now));
    CHECK(run(air, a, b, [&] { return *a.link.txPower() == -6.0; }));
    CHECK(hostA.txPowerDb() == -6.0);

    // A host from before PR #122: a bare DISCONNECTED.
    hostA.setReasons(false);
    a.events.clear();
    CHECK(a.link.call("K1ABC", now));
    CHECK(run(air, a, b, [&] { return a.link.sessionState() == HostLink::SessionState::None; }));
    CHECK(ended(a, ""));
}

int main()
{
    testTwoStations();
    testACut();
    testSnrs();
    testTheHostSaysNo();
    testReasonsAndTheIdleTimeout();
    return ragtime_test::finish();
}
