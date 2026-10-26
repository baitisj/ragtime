// ragtime_fake_air: a bench for trying Ragtime without Data2G. It runs
// fake data2g-hosts on one shared air (tests/FakeHost.h) and a few
// stations of its own that check in, chat and answer, so the station list,
// Globus and the Broadcast tab have something to show. The bots that take
// sessions answer a Call and ragchew: they answer what you type, and
// leave after you send 73.
//
//   ragtime_fake_air [--bots N] [--airtime MS] [--call CALL] [--fade] [--rate BPS]
//                    [--loss PCT] [--files] [--no-snr] [--no-cut] [--no-reasons]
//                    [--rig PORT [--dial HZ] [--dial-hop] [--rig-vfo] [--rig-swr X] [--rig-noise DB]]
//   ragtime_fake_air --replay FILE|FOLDER... [--from TIME] [--speed X]
//
// --call CALL has the first bot that takes sessions call CALL half a
// minute in, for an incoming session, and offer you its bulletin, a small
// text file, once the session has run 20 s. In any ragchew a bot offers
// its bulletin again when you mention a file ("send the file again"), and
// saves whatever you send it (in the temp folder) and says so. --fade puts the bots' sessions into
// a 45 s fade every two minutes, so Link quiet shows. --rate BPS carries
// session bytes at most that fast (default 400 bytes a second), so files
// take a while and show their progress. A call from host A
// or B is answered on its second try, so "try 2 of 5" shows.
//
// The bots seal what they broadcast and answer repair requests, and take
// broadcast files without asking. --loss PCT has hosts A and B lose that
// share of the group frames they hear, so a Ragtime on them asks for
// repairs; --files has the first bot broadcast a short bulletin file a
// minute in and every six minutes after.
//
// The hosts report SNRs as data2g-host has since Data2G PR #98: each bot
// is heard at its own strength, from +12 dB down to -11 dB, give or take
// 2 dB a burst, and hosts A and B at +5 dB. --no-snr has them report none,
// as hosts before it did. They take DATA2G ON and cut what a session holds
// (CLEANTXBUFFER), as data2g-host does since Data2G PR #118; --no-cut plays
// a host from before it. They say why a session ended (DISCONNECTED
// BUSY, IDLE, PEER...), and take TIMEOUT and TXPOWER, as data2g-host does
// since Data2G PR #122, hanging up a session quiet for the shorter of its
// two hosts' idle timeouts (the bots set Ragtime's day); --no-reasons
// plays a host from before it.
//
// --rig PORT plays a rigctld on that port (pick another than 4532 where a
// real one runs): it answers "\chk_vfo" with 0, "f" with the dial,
// 14.105 MHz or --dial HZ, and the meters "+l ALC", "+l SWR" and
// "+l RFPOWER_METER_WATTS" (ALC 30% to 60%, SWR 1.3 or --rig-swr X, 38 W),
// and the S-meter "+l STRENGTH": the band's noise, -40 dB over S9 (S2/S3)
// or --rig-noise DB, a few dB more now and then, and a station at -10 dB
// one reading in six. For the Ear it answers "+m" with USB and a
// 2400 Hz filter, and "+l PBT_IN" and "+l PBT_OUT" with the knob in the
// middle (0.5), and takes the Ear's settings, "M USB 1800" and
// "L PBT_IN 0.6510", printing each and answering with them after. It
// prints anything else it is sent (Ragtime should only ever ask those).
// --rig-vfo plays one started with --vfo: \chk_vfo is 1, and it answers
// "f currVFO" and "+l currVFO ALC", but not a bare "f".
// --dial-hop moves the dial to 14.250 MHz, outside the US data segments,
// for one minute in every three, so receive only shows.
//
// --replay plays a flight recording's host side back on host A's ports,
// instead of the air and its bots (tests/ReplayHost.h,
// docs/FLIGHT_RECORDER.md): a report's flight/ folder, or some of its
// files. --from 2026-10-25T16:20 starts at that recorded time (UTC), and
// --speed 4 plays four times as fast. It prints what it plays and what
// the Ragtime writes.
//
// Host A listens on 127.0.0.1:8100 (KISS) and 8300 (commands), host B on
// 8110 and 8310, so two Ragtimes (each with its own --config file) can talk
// to each other through it. Nothing here is Data2G's, and nothing goes on
// the radio. Ctrl-C stops it.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "FakeHost.h"
#include "ReplayHost.h"
#include "ragtime/broadcast/BroadcastChat.h"
#include "ragtime/broadcast/BroadcastFiles.h"
#include "ragtime/broadcast/Seals.h"
#include "ragtime/files/FileTraffic.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/link/FlightRecorder.h"
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

// A rigctld that knows the questions Ragtime asks: "\chk_vfo", "f", the
// dial frequency ("f currVFO" when it plays --vfo), the levels and "+m";
// and takes the Ear's filter settings, "M" and "L PBT_IN", "L PBT_OUT".
class FakeRig
{
public:
    FakeRig(int port, uint64_t dialHz, bool hop, bool vfo, double swr, int noise)
        : dialHz_(dialHz)
        , hop_(hop)
        , vfo_(vfo)
        , swr_(swr)
        , noise_(noise)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (listener_ < 0 || ::bind(listener_, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(listener_, 4) != 0)
            throw std::runtime_error("can't listen on " + std::to_string(port) + " for --rig");
        ::fcntl(listener_, F_SETFL, O_NONBLOCK);
    }

    ~FakeRig()
    {
        for (const Client& c : clients_) ::close(c.fd);
        if (listener_ >= 0) ::close(listener_);
    }

    void pump(uint64_t nowMs)
    {
        for (int fd; (fd = ::accept(listener_, nullptr, nullptr)) >= 0;) clients_.push_back(Client{fd, {}});
        for (Client& c : clients_)
        {
            std::vector<uint8_t> in;
            readSome(c.fd, in);
            c.partial.append(in.begin(), in.end());
            for (size_t eol; (eol = c.partial.find('\n')) != std::string::npos;)
            {
                std::string line = c.partial.substr(0, eol);
                c.partial.erase(0, eol + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string answer;
                if (line == "\\chk_vfo")
                {
                    answer = vfo_ ? "1\n" : "0\n";
                }
                else if (line.compare(0, vfo_ ? 11 : 3, vfo_ ? "+l currVFO " : "+l ") == 0)
                {
                    std::string level = line.substr(line.rfind(' ') + 1);
                    std::string head = "get_level: " + line.substr(3) + "\n";
                    char value[32] = "";
                    if (level == "ALC")
                        std::snprintf(value, sizeof value, "%.6f\n", 0.3 + 0.3 * (double)(nowMs / 250 % 7) / 6);
                    else if (level == "SWR")
                        std::snprintf(value, sizeof value, "%.6f\n", swr_);
                    else if (level == "RFPOWER_METER_WATTS")
                        std::snprintf(value, sizeof value, "%.6f\n", 38.0);
                    else if (level == "PBT_IN" || level == "PBT_OUT")
                        std::snprintf(value, sizeof value, "%.6f\n", level == "PBT_IN" ? pbtIn_ : pbtOut_);
                    else if (level == "STRENGTH")
                        std::snprintf(value, sizeof value, "%d\n",
                                      ++strengths_ % 6 == 0 ? -10 : noise_ + (int)(nowMs / 5000 % 4));
                    answer = head + (value[0] != 0 ? std::string(value) + "RPRT 0\n" : std::string("RPRT -11\n"));
                }
                else if (line == (vfo_ ? "+m currVFO" : "+m"))
                {
                    answer = "get_mode:" + std::string(vfo_ ? " currVFO" : "") + "\nMode: " + mode_ +
                             "\nPassband: " + std::to_string(width_) + "\nRPRT 0\n";
                }
                else if (filterSetting(line))
                {
                    std::printf("fake rigctld set: %s\n", line.c_str());
                    std::fflush(stdout);
                    answer = "RPRT 0\n";
                }
                else if (line == (vfo_ ? "f currVFO" : "f"))
                {
                    // One minute in three outside the data segments.
                    bool out = hop_ && nowMs / 60000 % 3 == 2;
                    answer = std::to_string(out ? 14250000ull : (unsigned long long)dialHz_) + "\n";
                }
                else
                {
                    std::printf("fake rigctld was sent \"%s\": Ragtime should only ask \"\\chk_vfo\", \"%s\" and "
                                "the levels and +m\n",
                                line.c_str(), vfo_ ? "f currVFO" : "f");
                    std::fflush(stdout);
                    // A --vfo rigctld waits for the VFO a bare "f" lacks.
                    if (!(vfo_ && line == "f")) answer = "RPRT -11\n";
                }
                if (!answer.empty()) sendAll(c.fd, answer.data(), answer.size());
            }
        }
    }

private:
    struct Client
    {
        int fd;
        std::string partial;
    };

    // "M USB 1800", "L PBT_IN 0.6510" (with currVFO for --vfo): taken.
    bool filterSetting(const std::string& line)
    {
        std::istringstream in(line);
        in.imbue(std::locale::classic());
        std::string command, vfo;
        in >> command;
        if (vfo_ && !(in >> vfo && vfo == "currVFO")) return false;
        if (command == "M")
        {
            std::string mode;
            int width = 0;
            if (!(in >> mode >> width) || width <= 0) return false;
            mode_ = mode;
            width_ = width;
            return true;
        }
        std::string level;
        double v = 0;
        if (command != "L" || !(in >> level >> v) || v < 0 || v > 1) return false;
        if (level == "PBT_IN") pbtIn_ = v;
        else if (level == "PBT_OUT") pbtOut_ = v;
        else return false;
        return true;
    }

    std::string mode_ = "USB";
    int width_ = 2400;
    double pbtIn_ = 0.5;
    double pbtOut_ = 0.5;
    int listener_ = -1;
    uint64_t dialHz_;
    bool hop_;
    bool vfo_;
    double swr_;
    int noise_;
    uint64_t strengths_ = 0;
    std::vector<Client> clients_;
};

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

bool mentionsAFile(std::string text)
{
    for (char& c : text) c = (char)std::tolower((unsigned char)c);
    return text.find("file") != std::string::npos || text.find("again") != std::string::npos
           || text.find("bulletin") != std::string::npos;
}

// A bot's folder, for its bulletin and what it is sent.
std::filesystem::path botFolder(const char* call)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path()
                                / ("ragtime-fake-air-" + std::to_string((long)::getpid())) / call;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string writeBulletin(const Profile& p)
{
    std::filesystem::path path = botFolder(p.call) / (std::string(p.call) + "-bulletin.txt");
    std::ofstream out(path, std::ios::binary);
    out << "QST de " << p.call << ": D2G net bulletin\n\n";
    for (int i = 1; i <= 60; i++)
        out << i << ". " << p.lines[i % 3] << ". Name " << p.name << ", locator "
            << (p.locator[0] != 0 ? p.locator : "unknown") << ", all stations welcome on the D2G net.\n";
    out << "\n73 de " << p.call << "\n";
    return files::utf8FromPath(path);
}

// A station of the bench's own: a HostLink and a BroadcastChat on its own
// fake host, over real sockets.
struct Bot
{
    Bot(FakeAir& air, const Profile& p, int airtimeMs, uint64_t startMs, std::mt19937& random)
        : host(air), link({p.call, p.sessions, false, link::HostLink::RAGCHEW_IDLE_TIMEOUT_S}), chat(link), seals(link, chat), broadcasts(link, chat, seals),
          session(link), traffic(files, history), profile(p), random(random)
    {
        session.setTraffic(&traffic);
        bulletin = writeBulletin(p);
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
        seals.setChain(p.call, broadcast::Seals::Chain());
        seals.setRandomSeed((uint32_t)random());
        chat.onItems([this](const std::vector<protocol::Record>& items, uint64_t burst, const std::string& mode,
                            uint64_t at) { seals.going(items, burst, mode, at); });
        broadcasts.setAutoReceive(files::utf8FromPath(botFolder(p.call)));
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
                if (!e.incoming && offersOnCall) offerMs = now + 20000;
            }
            else if (e.type == Type::Heard)
            {
                const session::Message* m = session.find(e.id);
                if (m == nullptr) continue;
                if (mentionsAFile(m->text))
                {
                    say("Sure, here comes my bulletin.", now + 3000);
                    offerMs = now + 5000;
                }
                else if (says73(m->text))
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
                offerMs = 0;
                host.fadeSession(false);
            }
        }
        // Files: saves what it is offered, and offers its bulletin when asked.
        for (const files::FileTraffic::Event& e : traffic.takeEvents())
        {
            const files::FileTransfer* t = files.transfer(e.id);
            if (t == nullptr) continue;
            if (e.type == files::FileTraffic::Event::Type::Offered && t->state == files::FileTransfer::State::Asking)
            {
                std::string error;
                std::string path = files::utf8FromPath(files::freeSavePath(botFolder(profile.call), t->name));
                if (!files.accept(e.id, t->kept > 0 ? t->keptPath : path, error)) files.decline(e.id);
            }
            else if (e.type == files::FileTraffic::Event::Type::Finished && !t->outgoing
                     && t->state == files::FileTransfer::State::Saved)
            {
                say("Got " + t->name + ", " + files::sizeText(t->size) + ", thanks!", now + 2000);
            }
        }
        if (offerMs != 0 && now >= offerMs && session.phase() == session::SessionChat::Phase::Connected)
        {
            std::string error;
            files.offer(session.peer(), bulletin, now, error);
            offerMs = 0;
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

    // A short bulletin, broadcast as a file.
    void share(uint64_t now)
    {
        std::filesystem::path path = botFolder(profile.call) / (std::string(profile.call) + "-bulletin-" + std::to_string(++shared)
                                               + ".txt");
        std::ofstream out(path, std::ios::trunc);
        out << "QST de " << profile.call << ": D2G net bulletin number " << shared << "\n\n";
        for (int i = 0; i < 40; i++)
            out << "Line " << (i + 1) << ": the net meets nightly; check in, pass traffic, and 73 to all.\n";
        out.close();
        std::string error;
        if (broadcasts.send(files::utf8FromPath(path), "", now, error) == 0) std::fprintf(stderr, "%s: %s\n", profile.call, error.c_str());
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
        // Pongs to the pings it hears.
        chat.tick(now);
        seals.tick(now);
        broadcasts.tick(now);
        broadcasts.takeEvents();
        broadcasts.takeFinished();
        traffic.setCuts(link.data2g());
        traffic.tick(now);

        for (const link::HostLink::Event& e : link.takeEvents())
        {
            session.onEvent(e, now);
            seals.onEvent(e, now);
            broadcasts.onEvent(e, now);
            std::vector<protocol::Record> heard = chat.onEvent(e, now);
            for (size_t i = 0; i < heard.size(); i++)
            {
                const protocol::Record r = heard[i];
                seals.onRecord(r, now);
                broadcasts.onRecord(r, now);
                // Lines rebuilt from parity are taken as heard.
                for (const protocol::Record& again : seals.takeRebuilt())
                {
                    chat.onRebuilt(again, now);
                    heard.push_back(again);
                }
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
        if (shareEveryMs != 0 && link.groupOpen() && now >= shareMs)
        {
            share(now);
            shareMs = now + shareEveryMs;
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
    broadcast::Seals seals;
    broadcast::BroadcastFiles broadcasts;
    session::SessionChat session;
    files::SessionFiles files;
    files::FileHistory history;
    files::FileTraffic traffic;
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
    bool offersOnCall = false;
    std::string bulletin;
    uint64_t offerMs = 0;
    uint64_t nextMs = 0;
    uint64_t checkInMs = 0;
    size_t line = 0;
    std::string replyTo;
    uint64_t replyMs = 0;
    uint64_t shareEveryMs = 0;
    uint64_t shareMs = 0;
    int shared = 0;
};

int replay(const std::vector<std::string>& paths, ReplayHost::Options options)
{
    std::vector<link::FlightLine> recording;
    std::string why;
    if (!loadRecording(paths, recording, why))
    {
        std::fprintf(stderr, "ragtime_fake_air: %s\n", why.c_str());
        return 1;
    }
    std::unique_ptr<ReplayHost> host;
    try
    {
        host = std::make_unique<ReplayHost>(recording, options, 8100, 8300);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "ragtime_fake_air: %s (is another bench or data2g-host running?)\n", e.what());
        return 1;
    }
    host->onLog = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });
    std::printf("Replaying %zu recorded connection%s on 127.0.0.1: KISS 8100, commands 8300, session data 8301,\n"
                "at %g times the recorded speed. Point a Ragtime at it; Ctrl-C to stop.\n",
                host->connections(), host->connections() == 1 ? "" : "s", options.speed);
    std::fflush(stdout);
    bool told = false;
    while (!stopping)
    {
        host->step(50);
        if (host->finished() && !told)
        {
            std::printf("Replay done: every recorded connection has played.\n");
            std::fflush(stdout);
            told = true;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    int bots = 4;
    int airtimeMs = 1500;
    std::string callWho;
    bool fades = false;
    int lossPct = 0;
    bool share = false;
    int rate = 400;
    bool snr = true;
    bool cuts = true;
    bool reasons = true;
    int rigPort = 0;
    uint64_t dialHz = 14105000;
    bool dialHop = false;
    bool rigVfo = false;
    double rigSwr = 1.3;
    int rigNoise = -40;
    std::vector<std::string> replayPaths;
    ReplayHost::Options replayOptions;
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
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            rate = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--loss") == 0 && i + 1 < argc)
            lossPct = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--files") == 0)
            share = true;
        else if (std::strcmp(argv[i], "--no-snr") == 0)
            snr = false;
        else if (std::strcmp(argv[i], "--no-cut") == 0)
            cuts = false;
        else if (std::strcmp(argv[i], "--no-reasons") == 0)
            reasons = false;
        else if (std::strcmp(argv[i], "--rig") == 0 && i + 1 < argc)
            rigPort = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dial") == 0 && i + 1 < argc)
            dialHz = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--dial-hop") == 0)
            dialHop = true;
        else if (std::strcmp(argv[i], "--rig-vfo") == 0)
            rigVfo = true;
        else if (std::strcmp(argv[i], "--rig-swr") == 0 && i + 1 < argc)
            rigSwr = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--rig-noise") == 0 && i + 1 < argc)
            rigNoise = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
        {
            while (i + 1 < argc && argv[i + 1][0] != '-') replayPaths.push_back(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc
                 && link::parseFlightTime(argv[i + 1], replayOptions.fromMs))
            i++;
        else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc && std::atof(argv[i + 1]) > 0)
            replayOptions.speed = std::atof(argv[++i]);
        else
        {
            std::fprintf(stderr, "usage: ragtime_fake_air [--bots N] [--airtime MS] [--call CALL] [--fade] "
                                 "[--rate BPS] [--loss PCT] [--files] [--no-snr] [--no-cut] [--no-reasons] "
                                 "[--rig PORT [--dial HZ] [--dial-hop] [--rig-vfo] [--rig-swr X] [--rig-noise DB]]\n"
                                 "       ragtime_fake_air --replay FILE|FOLDER... [--from 2026-10-25T16:20] "
                                 "[--speed X]\n");
            return 2;
        }
    }
    if (!replayPaths.empty()) return replay(replayPaths, replayOptions);
    const int maxBots = (int)(sizeof(PROFILES) / sizeof(PROFILES[0]));
    if (bots < 0) bots = 0;
    if (bots > maxBots) bots = maxBots;

    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });

    FakeAir air;
    std::unique_ptr<FakeHost> a, b;
    std::unique_ptr<FakeRig> rig;
    try
    {
        a = std::make_unique<FakeHost>(air, 2, 8100, 8300);
        b = std::make_unique<FakeHost>(air, 2, 8110, 8310);
        if (rigPort > 0) rig = std::make_unique<FakeRig>(rigPort, dialHz, dialHop, rigVfo, rigSwr, rigNoise);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "ragtime_fake_air: %s (is another bench or data2g-host running?)\n", e.what());
        return 1;
    }
    for (FakeHost* h : {a.get(), b.get()})
    {
        h->setAirtimeMs(airtimeMs);
        h->setSessionRate(rate);
        h->setAnswerOnCall(2);
        h->setIdleBroadcastMs(3000);
        h->setLoss(std::min(100, std::max(0, lossPct)) / 100.0);
        h->setReportsSnr(snr);
        h->setCuts(cuts);
        h->setReasons(reasons);
        h->setSignalDb(5.0, 2.0);
    }

    std::mt19937 random(12345);
    std::vector<std::unique_ptr<Bot>> stations;
    uint64_t start = nowMs();
    for (int i = 0; i < bots; i++)
    {
        stations.push_back(std::make_unique<Bot>(air, PROFILES[i], airtimeMs, start + 3000 + 2500 * (uint64_t)i,
                                                 random));
        stations.back()->fades = fades;
        stations.back()->host.setSessionRate(rate);
        // Each bot its own strength, strong to weak.
        static const double SIGNAL_DB[] = {12.0, -6.0, 3.0, -11.0, 8.0, -2.0, 15.0, -8.0};
        stations.back()->host.setReportsSnr(snr);
        stations.back()->host.setCuts(cuts);
        stations.back()->host.setReasons(reasons);
        stations.back()->host.setSignalDb(SIGNAL_DB[(size_t)i % (sizeof(SIGNAL_DB) / sizeof(SIGNAL_DB[0]))], 2.0);
    }
    if (share && !stations.empty())
    {
        stations.front()->shareEveryMs = 6 * 60 * 1000;
        stations.front()->shareMs = start + 60000;
    }
    for (auto& s : stations)
    {
        if (callWho.empty() || !s->profile.sessions) continue;
        s->callWho = callWho;
        s->callMs = start + 30000;
        s->offersOnCall = true;
        break;
    }

    std::printf("Fake data2g-hosts on 127.0.0.1: A KISS 8100, commands 8300, session data 8301;\n"
                "B KISS 8110, commands 8310, session data 8311.\n"
                "%d bot station%s on the air. Ctrl-C to stop.\n",
                bots, bots == 1 ? "" : "s");
    if (rig)
        std::printf("A rigctld%s on 127.0.0.1:%d, the dial at %llu Hz%s.\n", rigVfo ? " with --vfo" : "", rigPort,
                    (unsigned long long)dialHz, dialHop ? " (14.250 MHz one minute in three)" : "");
    std::fflush(stdout);
    while (!stopping)
    {
        air.step(20);
        uint64_t now = nowMs();
        for (auto& s : stations) s->pump(now);
        if (rig) rig->pump(now);
    }
    return 0;
}
