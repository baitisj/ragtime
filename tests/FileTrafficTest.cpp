// Session files in a session (package G): two stations, each a HostLink,
// a SessionChat and the session file engine plugged in as FileTraffic,
// with a host played by hand between them that reads what each writes,
// reports BUFFER, and carries so many bytes a step to the far end. Then
// the Files tab's rows, its log kept as text, and the words and numbers
// it shows. The clock is simulated; files go in a scratch folder.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/broadcast/Airtime.h"
#include "ragtime/files/FileRows.h"
#include "ragtime/files/FileTraffic.h"
#include "ragtime/files/SessionFiles.h"
#include "ragtime/link/Data2GLink.h"
#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"
#include "ragtime/session/SessionChat.h"

using namespace ragtime;
using namespace ragtime::files;
using link::HostLink;
using session::SessionChat;
using State = FileTransfer::State;
using Result = FinishedFile::Result;
using Look = FileRow::Look;

namespace
{

namespace fs = std::filesystem;

const uint64_t T0 = 1792627200000; // 2026-10-22 00:00 UTC

struct Scratch
{
    explicit Scratch(const std::string& name)
    {
        std::random_device random;
        dir = fs::temp_directory_path() / ("ragtime-traffic-test-" + name + "-" + std::to_string(random()));
        fs::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string path(const std::string& name) const { return utf8FromPath(dir / pathFromUtf8(name)); }

    std::string write(const std::string& name, size_t size, int seed = 7)
    {
        std::string content(size, '\0');
        for (size_t i = 0; i < size; i++) content[i] = (char)((i * (size_t)seed + i / 251) & 0xFF);
        std::ofstream out(dir / pathFromUtf8(name), std::ios::binary);
        out.write(content.data(), (std::streamsize)content.size());
        return path(name);
    }

    fs::path dir;
};

std::string readFile(const std::string& path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// One station: its host link, its session, and its files.
struct Station
{
    explicit Station(const std::string& call)
        : link({call, true, false}), chat(link), traffic(files, history)
    {
        SessionChat::Settings s;
        s.callsign = call;
        s.software = "Ragtime test";
        chat.setSettings(s);
        chat.setTraffic(&traffic);
    }

    // data2g: a host since Data2G PR #118, which takes DATA2G ON and cuts.
    void open(uint64_t now, bool data2g = false)
    {
        link.commandUp(now);
        say(data2g ? "OK\rOK\rOK\rOK\rOK\rBCAST PORT 2\rOK\r" : "OK\rOK\rWRONG\rOK\rOK\rBCAST PORT 2\rOK\r", now);
        link.takeCommandOutput();
        commands.clear();
        chat.dataPort(true, now);
        traffic.setCuts(link.data2g());
    }

    void say(const std::string& lines, uint64_t now)
    {
        link.commandBytes(lines.data(), lines.size(), now);
        for (const HostLink::Event& e : link.takeEvents()) chat.onEvent(e, now);
        commands += link.takeCommandOutput();
    }

    // The CLEANTXBUFFER lines written since the last call, by their offsets.
    std::vector<uint64_t> cleans()
    {
        commands += link.takeCommandOutput();
        std::vector<uint64_t> out;
        size_t at = 0;
        while (at < commands.size())
        {
            size_t cr = commands.find('\r', at);
            if (cr == std::string::npos) break;
            std::string line = commands.substr(at, cr - at);
            at = cr + 1;
            lines.push_back(line);
            if (line.compare(0, 14, "CLEANTXBUFFER ") == 0) out.push_back(std::stoull(line.substr(14)));
        }
        commands.erase(0, at);
        return out;
    }

    std::vector<FileTraffic::Event> events(FileTraffic::Event::Type type)
    {
        std::vector<FileTraffic::Event> out;
        for (const FileTraffic::Event& e : traffic.takeEvents())
            if (e.type == type) out.push_back(e);
        return out;
    }

    const FileTransfer* transfer(bool outgoing) const
    {
        const FileTransfer* last = nullptr;
        for (const FileTransfer& t : files.transfers())
        {
            if (t.outgoing == outgoing) last = files.transfer(t.id);
        }
        return last;
    }

    HostLink link;
    SessionChat chat;
    SessionFiles files;
    FileHistory history;
    FileTraffic traffic;
    std::vector<uint8_t> inFlight;  // read by the host, not yet carried
    size_t mostWaiting = 0;         // the most file and chat bytes ever waiting at the host
    std::string commands;           // the link's lines to the host, not yet looked at
    std::vector<std::string> lines; // and those looked at

    // A host that cuts (Pair::cuts): its bytes on air, not yet arrived.
    uint64_t hostRead = 0;          // host offsets: every byte read this session
    std::vector<uint8_t> onAir;
    uint64_t onAirFrom = 0;
    bool limbo = false;             // cut while on air: it settles as it arrives
    size_t carriedSinceCut = 0;     // bytes to cross since the last cut asked
    size_t lagBytes = 0;            // the next cut leaves this many, as if not yet read
    std::vector<uint8_t> socket;    // and here they are, read the step after
};

// Two stations and the air between them: each step, what each writes is
// read by its host at once, and up to perStep bytes of it cross.
struct Pair
{
    // cuts: both hosts are Data2G's since PR #118. Their bursts then take
    // a step on air before they arrive, so a cut can catch one there.
    explicit Pair(bool cutting = false) : a("AG7EW"), b("W1AW"), cuts(cutting)
    {
        a.open(now, cutting);
        b.open(now, cutting);
    }

    // What becomes of a burst on air when a cut catches it.
    enum class Limbo
    {
        Arrives,
        Lost,
        Half,
    };

    void buffer(Station& s)
    {
        s.say("BUFFER " + std::to_string(s.inFlight.size() + s.onAir.size()) + "\r", now);
    }

    void carryCutting(Station& from, Station& to)
    {
        from.inFlight.insert(from.inFlight.end(), from.socket.begin(), from.socket.end());
        from.hostRead += from.socket.size();
        from.socket.clear();
        std::vector<uint8_t> out = from.chat.takeDataOutput(now);
        if (!out.empty())
        {
            from.inFlight.insert(from.inFlight.end(), out.begin(), out.end());
            from.hostRead += out.size();
            from.mostWaiting = std::max(from.mostWaiting, from.inFlight.size() + from.onAir.size());
            buffer(from);
        }
        for (uint64_t n : from.cleans())
        {
            (void)n;
            if (from.inFlight.empty() && from.onAir.empty())
            {
                from.say("CLEANTXBUFFER BUFFEREMPTY\r", now);
                continue;
            }
            uint64_t dropFrom = from.hostRead - from.inFlight.size();
            // Bytes still on their way to the host are kept, and follow
            // the cut (lagBytes: once, so many).
            size_t keep = from.lagBytes < from.inFlight.size() ? from.lagBytes : 0;
            from.lagBytes = 0;
            from.socket.assign(from.inFlight.end() - (long)keep, from.inFlight.end());
            from.inFlight.clear();
            from.hostRead -= keep;
            uint64_t limboTo = from.onAirFrom + from.onAir.size();
            from.limbo = !from.onAir.empty();
            uint64_t limboFrom = from.limbo ? from.onAirFrom : dropFrom;
            if (!from.limbo) limboTo = dropFrom;
            from.say("CLEANTXBUFFER OK " + std::to_string(dropFrom) + " " + std::to_string(from.hostRead) + " "
                         + std::to_string(limboFrom) + " " + std::to_string(limboTo) + "\r",
                     now);
            buffer(from);
        }
        if (fading) return;
        // The burst on air arrives, all of it or, cut while on air, what
        // the cut leaves.
        if (!from.onAir.empty())
        {
            size_t arrives = from.onAir.size();
            if (from.limbo) arrives = limbo == Limbo::Arrives ? arrives : limbo == Limbo::Lost ? 0 : arrives / 2;
            to.chat.dataBytes(from.onAir.data(), arrives, now);
            from.carriedSinceCut += arrives;
            if (from.limbo)
            {
                from.limbo = false;
                from.say("CLEANTXBUFFER SETTLED " + std::to_string(from.onAirFrom) + " "
                             + std::to_string(from.onAirFrom + arrives) + " "
                             + std::to_string(from.onAirFrom + from.onAir.size()) + "\r",
                         now);
            }
            from.onAir.clear();
        }
        // The next goes on air.
        size_t n = std::min(perStep, from.inFlight.size());
        from.onAirFrom = from.hostRead - from.inFlight.size();
        from.onAir.assign(from.inFlight.begin(), from.inFlight.begin() + (long)n);
        from.inFlight.erase(from.inFlight.begin(), from.inFlight.begin() + (long)n);
        buffer(from);
    }

    void connect()
    {
        CHECK(a.chat.call("W1AW", now));
        a.say("OK\rCONNECTED AG7EW W1AW 2300\r", now);
        b.say("CONNECTED AG7EW W1AW 2300\r", now);
        step(0);
    }

    void carry(Station& from, Station& to)
    {
        if (cuts)
        {
            carryCutting(from, to);
            return;
        }
        std::vector<uint8_t> out = from.chat.takeDataOutput(now);
        if (!out.empty())
        {
            from.inFlight.insert(from.inFlight.end(), out.begin(), out.end());
            from.mostWaiting = std::max(from.mostWaiting, from.inFlight.size());
            from.say("BUFFER " + std::to_string(from.inFlight.size()) + "\r", now);
        }
        if (fading || from.inFlight.empty()) return;
        size_t n = std::min(perStep, from.inFlight.size());
        std::vector<uint8_t> crossing(from.inFlight.begin(), from.inFlight.begin() + (long)n);
        from.inFlight.erase(from.inFlight.begin(), from.inFlight.begin() + (long)n);
        to.chat.dataBytes(crossing.data(), crossing.size(), now);
        from.say("BUFFER " + std::to_string(from.inFlight.size()) + "\r", now);
    }

    void step(uint64_t ms = 1000)
    {
        now += ms;
        for (Station* s : {&a, &b})
        {
            s->link.tick(now);
            s->chat.tick(now);
            s->traffic.tick(now);
            for (const HostLink::Event& e : s->link.takeEvents()) s->chat.onEvent(e, now);
        }
        carry(a, b);
        carry(b, a);
    }

    void steps(int n, uint64_t ms = 1000)
    {
        for (int i = 0; i < n; i++) step(ms);
    }

    // Steps until the transfer of a's (or b's) ends, at most n steps.
    bool until(const std::function<bool()>& done, int n = 2000)
    {
        for (int i = 0; i < n && !done(); i++) step();
        return done();
    }

    void disconnect()
    {
        a.say("DISCONNECTED\r", now);
        b.say("DISCONNECTED\r", now);
        a.inFlight.clear();
        b.inFlight.clear();
        a.onAir.clear();
        b.onAir.clear();
        step(0);
    }

    uint64_t now = T0;
    Station a;
    Station b;
    bool cuts = false;
    Limbo limbo = Limbo::Half;
    size_t perStep = 100000;
    bool fading = false;
};

//-------------------------------------------------------------------------

void testAFileGoesAndIsSaved()
{
    Scratch scratch("saved");
    Pair p;
    std::string sent = scratch.write("FRED.TXT", 7000);
    std::string error;
    uint64_t id = p.a.files.offer("W1AW", sent, p.now, error);
    CHECK(id != 0);

    // Queued for a session with W1AW, and not offered without one.
    std::vector<FileRow> rows = p.a.traffic.rows(p.now);
    CHECK(rows.size() == 1 && rows[0].state == "Waiting for a session with W1AW" && rows[0].look == Look::Waiting);

    p.connect();
    p.steps(2);
    std::vector<FileTraffic::Event> offered = p.b.events(FileTraffic::Event::Type::Offered);
    CHECK_EQ(offered.size(), (size_t)1);
    const FileTransfer* theirs = p.b.transfer(false);
    CHECK(theirs != nullptr && theirs->state == State::Asking && theirs->name == "FRED.TXT" && theirs->size == 7000);
    CHECK(p.a.transfer(true)->state == State::Offered);
    rows = p.a.traffic.rows(p.now);
    CHECK(rows.size() == 1 && rows[0].state == "Waiting for answer" && rows[0].expiresAtMs > p.now);
    rows = p.b.traffic.rows(p.now);
    CHECK(rows.size() == 1 && rows[0].state == "Waiting for your answer" && !rows[0].outgoing
          && rows[0].peer == "AG7EW");
    CHECK(p.a.traffic.busy("W1AW") && p.b.traffic.busy("AG7EW"));

    std::string saveAs = scratch.path("saved.txt");
    CHECK(p.b.files.accept(theirs->id, saveAs, error));
    CHECK(p.until([&]() { return p.a.transfer(true)->state == State::Delivered; }));
    CHECK_EQ(readFile(saveAs), readFile(sent));
    CHECK(p.b.transfer(false)->state == State::Saved);
    CHECK(!p.a.traffic.busy("W1AW") && p.a.traffic.rows(p.now).empty());

    // Both ends' logs have it.
    CHECK_EQ(p.a.history.entries().size(), (size_t)1);
    CHECK_EQ(p.b.history.entries().size(), (size_t)1);
    if (!p.a.history.entries().empty() && !p.b.history.entries().empty())
    {
        const FinishedFile& ours = p.a.history.entries().back();
        const FinishedFile& got = p.b.history.entries().back();
        CHECK(ours.result == Result::Sent && ours.outgoing && ours.peer == "W1AW" && ours.path == sent);
        CHECK(got.result == Result::Saved && !got.outgoing && got.peer == "AG7EW" && got.path == saveAs);
        CHECK_EQ(resultLabel(ours), std::string("Sent"));
        CHECK(got.size == 7000 && got.name == "FRED.TXT" && got.via == Via::Session);
        // The bps column: all of it crossed, over some seconds.
        CHECK(ours.movedBytes == 7000 && got.movedBytes == 7000);
        CHECK(got.movedMs >= 1000 && !bpsText(got).empty());
    }
    CHECK_EQ(p.a.events(FileTraffic::Event::Type::Finished).size(), (size_t)1);
}

void testFileBytesArePaced()
{
    // A slow link: 100 bytes a second. The host never holds much more
    // than the window, and a message typed mid-file arrives long before
    // the file ends.
    Scratch scratch("paced");
    Pair p;
    p.perStep = 100;
    std::string sent = scratch.write("BIG.BIN", 60000);
    std::string error;
    p.a.files.offer("W1AW", sent, p.now, error);
    p.connect();
    p.steps(3);
    const FileTransfer* theirs = p.b.transfer(false);
    CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
    p.steps(120);
    CHECK(p.a.transfer(true)->state == State::Sending);
    CHECK(p.a.mostWaiting <= 100 * FileTraffic::WINDOW_MS / 1000 + 300);

    // The rate settles near 100 bytes a second, and the window with it.
    FileRow row = p.a.traffic.rows(p.now)[0];
    CHECK(row.look == Look::Moving && row.state == "QRL");
    CHECK(row.bytesPerSecond > 80 && row.bytesPerSecond < 120);
    CHECK(row.finishAtMs > p.now + 400000 && row.finishAtMs < p.now + 700000);
    CHECK_EQ(p.a.traffic.window(), FileTraffic::MIN_WINDOW); // 10 s at 100 B/s is under 1 KB
    CHECK(row.done > 5000 && row.done <= row.handed && row.handed <= row.size);

    size_t heard = 0;
    p.a.chat.send("Is it coming through?", p.now);
    for (int i = 0; i < 120 && heard == 0; i++)
    {
        p.step();
        for (const SessionChat::Event& e : p.b.chat.takeEvents()) heard += e.type == SessionChat::Event::Type::Heard;
    }
    CHECK_EQ(heard, (size_t)1);
    CHECK(p.a.transfer(true)->state == State::Sending);
}

void testTheWindowFollowsTheMode()
{
    // Two of the mode's longest session bursts (12 s on OFDM, 24 s on
    // FSK), between 1 and 8 KB.
    using broadcast::findMode;
    link::ModeInfo fast = findMode({}, "w48-16qam-r1/2");
    link::ModeInfo qpsk = findMode({}, "qpsk-r1/3");
    link::ModeInfo narrow = findMode({}, "n10-16qam-r1/2");
    link::ModeInfo fsk = findMode({}, "fsk32r62-r1/3");
    CHECK(fast.valid() && qpsk.valid() && narrow.valid() && fsk.valid());
    CHECK_EQ(FileTraffic::burstBytes(fast), (uint64_t)236 * 19);  // 1.21 s, then 0.58 s a codeword
    CHECK_EQ(FileTraffic::burstBytes(qpsk), (uint64_t)76 * 9);
    CHECK_EQ(FileTraffic::burstBytes(narrow), (uint64_t)121 * 7);
    CHECK_EQ(FileTraffic::burstBytes(fsk), (uint64_t)38 * 7);    // 24 s on FSK
    CHECK_EQ(FileTraffic::burstBytes(link::ModeInfo()), (uint64_t)0);
    // A mode slow enough that one codeword is all a burst holds.
    link::ModeInfo crawl = fsk;
    crawl.secondsAtOne = 30.0;
    crawl.secondsAtMax = 90.0;
    CHECK_EQ(FileTraffic::burstBytes(crawl), (uint64_t)38);
    // One so fast that every codeword fits.
    link::ModeInfo dash = fast;
    dash.secondsAtMax = 5.0;
    CHECK_EQ(FileTraffic::burstBytes(dash), (uint64_t)236 * 64);

    SessionFiles files;
    FileHistory history;
    FileTraffic traffic(files, history);
    CHECK_EQ(traffic.window(), FileTraffic::START_WINDOW);
    traffic.setMode(fast);
    CHECK_EQ(traffic.window(), FileTraffic::MAX_WINDOW);
    traffic.setMode(qpsk);
    CHECK_EQ(traffic.window(), (uint64_t)2 * 76 * 9);
    traffic.setMode(narrow);
    CHECK_EQ(traffic.window(), (uint64_t)2 * 121 * 7);
    traffic.setMode(fsk);
    CHECK_EQ(traffic.window(), FileTraffic::MIN_WINDOW);
    traffic.setMode(link::ModeInfo());
    CHECK_EQ(traffic.window(), FileTraffic::START_WINDOW);
}

void testAFastModeKeepsTheHostFull()
{
    // A fast mode: the host is handed the whole 8 KB window at once, not
    // the 1 KB a measured rate starts from, and a fade doesn't shrink it.
    Scratch scratch("fast");
    Pair p;
    p.perStep = 2000;
    p.a.traffic.setMode(broadcast::findMode({}, "w48-16qam-r1/2"));
    std::string sent = scratch.write("FAST.BIN", 60000);
    std::string error;
    p.a.files.offer("W1AW", sent, p.now, error);
    p.connect();
    p.steps(3);
    const FileTransfer* theirs = p.b.transfer(false);
    CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
    p.steps(3);
    CHECK(p.a.mostWaiting > FileTraffic::MAX_WINDOW - 1024);
    CHECK(p.a.mostWaiting <= FileTraffic::MAX_WINDOW + 300);

    p.fading = true;
    p.steps(40);
    p.fading = false;
    CHECK_EQ(p.a.traffic.window(), FileTraffic::MAX_WINDOW);
    CHECK(p.until([&]() { return p.a.transfer(true)->state == State::Delivered; }));
    CHECK_EQ(readFile(scratch.path("got.bin")), readFile(sent));
}

void testACancelIsCut()
{
    // A host that cuts: a cancel drops the file bytes it holds, so the far
    // end hears it after one burst, not the whole window, and both ends
    // count the same part.
    for (Pair::Limbo limbo : {Pair::Limbo::Arrives, Pair::Limbo::Lost, Pair::Limbo::Half})
    {
        Scratch scratch("cut");
        Pair p(true);
        p.limbo = limbo;
        p.perStep = 300;
        // Once, some bytes reach the host after the cut: cut again.
        if (limbo == Pair::Limbo::Half) p.a.lagBytes = 700;
        CHECK(p.a.link.data2g() && p.a.traffic.cuts());
        std::string sent = scratch.write("CUT.BIN", 40000, 7);
        std::string error;
        uint64_t id = p.a.files.offer("W1AW", sent, p.now, error);
        p.connect();
        p.steps(3);
        const FileTransfer* theirs = p.b.transfer(false);
        CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
        p.steps(12);
        // The whole window is with the host from the start.
        CHECK(p.a.mostWaiting > FileTraffic::MAX_WINDOW - 1024);
        CHECK(p.a.transfer(true)->state == State::Sending);

        CHECK(p.a.files.cancel(id));
        int steps = 0;
        CHECK(p.until([&]() { steps++; return p.b.transfer(false)->state == State::CancelledThere; }, 40));
        CHECK(steps <= 8);  // not 8 KB at 300 bytes a step
        CHECK_EQ(p.a.chat.cuts(), 1);
        CHECK(p.a.chat.cutBytes() > 4000);
        CHECK(!p.b.chat.foreign() && !p.a.chat.cutting());

        // The stream is whole: a message still goes through.
        p.a.chat.send("still here?", p.now);
        size_t heard = 0;
        for (int i = 0; i < 20 && heard == 0; i++)
        {
            p.step();
            for (const SessionChat::Event& e : p.b.chat.takeEvents()) heard += e.type == SessionChat::Event::Type::Heard;
        }
        CHECK_EQ(heard, (size_t)1);
        CHECK(!p.b.chat.foreign() && p.b.chat.reader().malformed() == 0);

        // Both ends hold the same part once the last of it is acknowledged.
        p.steps(5);
        std::vector<FileRow> ours = p.a.traffic.rows(p.now);
        std::vector<FileRow> theirsRows = p.b.traffic.rows(p.now);
        CHECK(ours.size() == 1 && theirsRows.size() == 1);
        if (ours.size() == 1 && theirsRows.size() == 1)
        {
            CHECK_EQ(ours[0].state, std::string("Part sent, cancelled"));
            CHECK_EQ(ours[0].done, theirsRows[0].done);
            CHECK_EQ(ours[0].handed, ours[0].done);
            CHECK(theirsRows[0].done > 0 && theirsRows[0].done < 40000);
        }
    }
}

void testAMessageCutsIn()
{
    // A message typed under a file goes ahead of the file bytes the host
    // holds, and the file carries on behind it, whole.
    for (Pair::Limbo limbo : {Pair::Limbo::Arrives, Pair::Limbo::Lost, Pair::Limbo::Half})
    {
        Scratch scratch("cutin");
        Pair p(true);
        p.limbo = limbo;
        p.perStep = 300;
        std::string sent = scratch.write("BIG.BIN", 30000, 11);
        std::string error;
        p.a.files.offer("W1AW", sent, p.now, error);
        p.connect();
        p.steps(3);
        const FileTransfer* theirs = p.b.transfer(false);
        CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
        p.steps(10);
        for (int m = 0; m < 3; m++)
        {
            uint64_t id = p.a.chat.send("over the file " + std::to_string(m), p.now);
            CHECK(id != 0);
            size_t heard = 0;
            int steps = 0;
            for (; steps < 30 && heard == 0; steps++)
            {
                p.step();
                for (const SessionChat::Event& e : p.b.chat.takeEvents())
                    heard += e.type == SessionChat::Event::Type::Heard;
            }
            CHECK_EQ(heard, (size_t)1);
            CHECK(steps <= 6);
            p.steps(4);
            const session::Message* mine = p.a.chat.find(id);
            CHECK(mine != nullptr && mine->state == session::Message::State::Delivered);
            p.steps(6);
        }
        CHECK(p.a.chat.cuts() >= 3);
        CHECK(p.until([&]() { return p.b.transfer(false)->state == State::Saved; }));
        CHECK_EQ(readFile(scratch.path("got.bin")), readFile(sent));
        CHECK(p.until([&]() { return p.a.transfer(true)->state == State::Delivered; }, 20));
        CHECK(!p.b.chat.foreign() && p.b.chat.reader().malformed() == 0);
    }
}

void testLeavingCutsFirst()
{
    // Leave under a file: the bytes the host holds are cut, and the
    // farewell goes after one burst.
    Scratch scratch("cutleave");
    Pair p(true);
    p.perStep = 300;
    std::string error;
    p.a.files.offer("W1AW", scratch.write("BIG.BIN", 30000, 13), p.now, error);
    p.connect();
    p.steps(3);
    const FileTransfer* theirs = p.b.transfer(false);
    CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
    p.steps(10);
    CHECK(p.a.chat.leave(p.now));
    // DISCONNECT once the host has read the farewell, the file bytes cut.
    auto disconnected = [&]() {
        p.a.cleans();
        return std::find(p.a.lines.begin(), p.a.lines.end(), "DISCONNECT") != p.a.lines.end();
    };
    CHECK(p.until(disconnected, 10));
    CHECK_EQ(p.a.chat.cuts(), 1);
    CHECK(p.until([&]() { return p.a.inFlight.empty() && p.a.onAir.empty(); }, 10));
    CHECK(p.b.transfer(false)->state == State::CancelledThere);
    p.disconnect();
    CHECK(p.b.chat.ending() == SessionChat::Ending::LeftThere);
}

void testAnOlderHostIsNotCut()
{
    // A host from before Data2G PR #118: no cuts, and the window as before.
    Scratch scratch("nocut");
    Pair p;
    p.perStep = 300;
    std::string error;
    uint64_t id = p.a.files.offer("W1AW", scratch.write("BIG.BIN", 30000, 17), p.now, error);
    p.connect();
    p.steps(3);
    const FileTransfer* theirs = p.b.transfer(false);
    CHECK(theirs != nullptr && p.b.files.accept(theirs->id, scratch.path("got.bin"), error));
    p.steps(5);
    CHECK(!p.a.traffic.cuts() && p.a.traffic.window() < FileTraffic::MAX_WINDOW);
    p.a.files.cancel(id);
    p.a.chat.send("hello", p.now);
    p.steps(5);
    CHECK_EQ(p.a.chat.cuts(), 0);
    CHECK(p.a.cleans().empty());
}

void testStalledWhileTheLinkFades()
{
    Scratch scratch("stalled");
    Pair p;
    p.perStep = 500;
    std::string error;
    p.a.files.offer("W1AW", scratch.write("F.BIN", 50000), p.now, error);
    p.connect();
    p.steps(3);
    p.b.files.accept(p.b.transfer(false)->id, scratch.path("f.bin"), error);
    p.steps(10);
    CHECK(p.a.traffic.rows(p.now)[0].look == Look::Moving);
    p.fading = true;
    p.steps(29);
    CHECK(p.a.traffic.rows(p.now)[0].look == Look::Moving);
    p.steps(51);
    FileRow row = p.a.traffic.rows(p.now)[0];
    CHECK(row.look == Look::Stalled && contains(row.state, "Stalled 1:"));
    CHECK(row.finishAtMs == 0);
    FileRow theirs = p.b.traffic.rows(p.now)[0];
    CHECK(theirs.look == Look::Stalled);
    p.fading = false;
    p.steps(3);
    CHECK(p.a.traffic.rows(p.now)[0].look == Look::Moving);
}

void testAStationThatTakesNoFiles()
{
    // The far end's Hello lacks session files: the file queued for it is
    // never offered, and ends Not taken.
    Scratch scratch("nottaken");
    Station a("AG7EW");
    uint64_t now = T0;
    a.open(now);
    std::string error;
    a.files.offer("W1AW", scratch.write("X.TXT", 100), now, error);
    CHECK(a.chat.call("W1AW", now));
    a.say("OK\rCONNECTED AG7EW W1AW 2300\r", now);
    std::vector<uint8_t> first = a.chat.takeDataOutput(now);

    protocol::Numbering far("W1AW");
    protocol::Hello hello;
    hello.takes = protocol::TAKES_TEXT | protocol::TAKES_STATUS;
    std::vector<uint8_t> stream = protocol::streamOpening(far.record(hello, (uint32_t)(now / 1000)));
    a.chat.dataBytes(stream.data(), stream.size(), now);
    std::vector<uint8_t> more = a.chat.takeDataOutput(now + 1000);
    a.traffic.tick(now + 1000);

    std::vector<protocol::Record> records;
    protocol::StreamReader reader;
    first.insert(first.end(), more.begin(), more.end());
    reader.feed(first, records);
    bool offered = std::any_of(records.begin(), records.end(),
                               [](const protocol::Record& r) { return r.as<protocol::FileOffer>() != nullptr; });
    CHECK(!offered);
    CHECK(a.transfer(true)->state == State::NotTaken);
    CHECK(!a.history.entries().empty() && a.history.entries().back().result == Result::NotTaken);
    if (!a.history.entries().empty())
        CHECK_EQ(resultLabel(a.history.entries().back()), std::string("Not taken: W1AW takes no files"));
}

void testOffersWaitForTheHello()
{
    // Nothing is offered before the far end's Hello says it takes files.
    Scratch scratch("hello");
    Station a("AG7EW");
    uint64_t now = T0;
    a.open(now);
    std::string error;
    a.files.offer("W1AW", scratch.write("X.TXT", 100), now, error);
    CHECK(a.chat.call("W1AW", now));
    a.say("OK\rCONNECTED AG7EW W1AW 2300\r", now);
    std::vector<uint8_t> out = a.chat.takeDataOutput(now);
    out = a.chat.takeDataOutput(now + 5000);
    CHECK(out.empty());
    CHECK(a.transfer(true)->state == State::Waiting);
    CHECK_EQ(a.traffic.rows(now)[0].state, std::string("Queued"));
}

void testLeavingKeepsThePartAndTheQueue()
{
    Scratch scratch("leave");
    Pair p;
    p.perStep = 2000;
    std::string first = scratch.write("ONE.BIN", 30000, 3);
    std::string second = scratch.write("TWO.BIN", 500, 5);
    std::string error;
    uint64_t one = p.a.files.offer("W1AW", first, p.now, error);
    uint64_t two = p.a.files.offer("W1AW", second, p.now, error);
    // The second goes first.
    CHECK(p.a.files.reorder(two, -1));
    CHECK(!p.a.files.reorder(two, -1));
    CHECK(p.a.files.reorder(two, 1) && p.a.files.reorder(two, -1));

    p.connect();
    p.steps(3);
    const FileTransfer* t = p.b.transfer(false);
    CHECK(t != nullptr && t->name == "TWO.BIN");
    // Saved under other names: on Windows "two.bin" is TWO.BIN itself.
    p.b.files.accept(t->id, scratch.path("two.got"), error);
    CHECK(p.until([&]() { return p.a.files.transfer(two)->state == State::Delivered; }));
    p.steps(3);
    t = p.b.transfer(false);
    CHECK(t != nullptr && t->name == "ONE.BIN");
    std::string saveAs = scratch.path("one.got");
    p.b.files.accept(t->id, saveAs, error);
    p.steps(4);
    CHECK(p.a.files.transfer(one)->state == State::Sending);

    // A third queued behind it stays queued when we leave.
    uint64_t three = p.a.files.offer("W1AW", scratch.write("THREE.TXT", 50), p.now, error);
    CHECK(p.a.chat.trafficBusy());
    p.a.chat.leave(p.now);
    // DISCONNECT waits for what was sent to be acknowledged.
    CHECK(p.until([&]() { return p.a.inFlight.empty(); }));
    p.disconnect();
    CHECK(p.a.files.transfer(one)->state == State::Cancelled);
    CHECK(p.a.files.transfer(three)->state == State::Waiting);
    CHECK(p.b.transfer(false)->state == State::CancelledThere);
    CHECK_EQ(p.b.files.keptCount(), (size_t)1);
    CHECK(fs::exists(pathFromUtf8(saveAs + ".part")));
    std::vector<FileRow> rows = p.b.traffic.rows(p.now);
    CHECK(rows.size() == 1 && rows[0].look == Look::Kept && rows[0].state == "Part kept, cancelled by AG7EW");
    if (rows.size() == 1)
    {
        // Named as offered, not as saved.
        CHECK(rows[0].done > 0 && rows[0].done < 30000 && rows[0].name == "ONE.BIN");
        CHECK_EQ(rows[0].expiresAtMs, p.now + SessionFiles::KEPT_MS);
    }
    // A's end of it isn't finished either: the part sent stays with what
    // B holds, and Completed has only TWO.BIN.
    uint64_t heldThere = rows.empty() ? 0 : rows[0].done;
    rows = p.a.traffic.rows(p.now);
    CHECK(rows.size() == 2 && rows[0].state == "Waiting for a session with W1AW");
    if (rows.size() == 2)
    {
        CHECK(rows[1].look == Look::Kept && rows[1].outgoing && rows[1].name == "ONE.BIN");
        CHECK(rows[1].state == "Part sent, cancelled");
        CHECK_EQ(rows[1].done, heldThere);
        CHECK_EQ(rows[1].handed, heldThere);
    }
    CHECK(p.a.history.entries().size() == 1 && p.a.history.entries().back().name == "TWO.BIN");
    std::vector<KeptPart> sentParts = p.a.files.keptParts();
    CHECK(sentParts.size() == 1 && sentParts[0].outgoing && sentParts[0].path == first);
    CHECK(sentParts.size() == 1 && sentParts[0].hasEnding && sentParts[0].ending.result == Result::Cancelled);
    CHECK_EQ(p.a.files.keptCount(), (size_t)0);
    // B's file isn't finished while its part is kept: Completed has only
    // TWO.BIN, and the ending waits with the part.
    CHECK(p.b.history.entries().size() == 1 && p.b.history.entries().back().name == "TWO.BIN");
    std::vector<FileTraffic::Event> ended = p.b.events(FileTraffic::Event::Type::Finished);
    CHECK(!ended.empty() && ended.back().partKept && ended.back().historyId == 0
          && ended.back().finished.result == Result::CancelledThere);
    CHECK(p.b.files.keptParts().size() == 1 && p.b.files.keptParts()[0].hasEnding
          && p.b.files.keptParts()[0].ending.result == Result::CancelledThere);

    // B restarts: its log keeps the part, and the same file carries on
    // from it next time.
    std::string log = saveFilesText(p.b.history, p.b.files.keptParts());
    FileHistory history;
    std::vector<KeptPart> kept;
    CHECK(loadFilesText(log, history, kept));
    CHECK(kept.size() == 1 && history.entries().size() == 1);
    CHECK(!kept.empty() && kept[0].hasEnding && resultLabel(kept[0].ending) == "Cancelled by AG7EW");
    SessionFiles restarted;
    restarted.restoreKept(kept, p.now + 3600000);
    CHECK_EQ(restarted.keptCount(), (size_t)1);

    // A fresh pair, B with the restored part: the same file offered again.
    Pair q;
    q.now = p.now + 60000;
    q.b.files.restoreKept(kept, q.now);
    q.a.files.offer("W1AW", first, q.now, error);
    q.connect();
    q.steps(3);
    t = q.b.transfer(false);
    CHECK(t != nullptr && t->kept > 0 && t->keptPath == saveAs);
    uint64_t keptBytes = t != nullptr ? t->kept : 0;
    if (t != nullptr) CHECK(q.b.files.accept(t->id, t->keptPath, error));
    CHECK(q.until([&]() { return q.b.transfer(false)->state == State::Saved; }));
    CHECK_EQ(q.b.transfer(false)->resumedFrom, keptBytes);
    CHECK_EQ(readFile(saveAs), readFile(first));
    CHECK_EQ(q.b.files.keptCount(), (size_t)0);
    // Carried on and saved: one entry in Completed, Saved.
    q.steps(2);
    CHECK(q.b.history.entries().size() == 1 && q.b.history.entries()[0].result == Result::Saved);
}

void testKeptPartsMenu()
{
    Scratch scratch("kept");
    Pair p;
    p.perStep = 1000;
    std::string error;
    p.a.files.offer("W1AW", scratch.write("K.BIN", 20000), p.now, error);
    p.connect();
    p.steps(3);
    p.b.files.accept(p.b.transfer(false)->id, scratch.path("k.bin"), error);
    p.steps(5);
    p.disconnect();
    std::vector<FileRow> rows = p.b.traffic.rows(p.now);
    CHECK(rows.size() == 1 && rows[0].look == Look::Kept);
    if (rows.size() != 1) return;
    uint64_t until = rows[0].expiresAtMs;
    CHECK(p.b.files.keepLonger(rows[0].id, SessionFiles::KEPT_MS, p.now));
    CHECK_EQ(p.b.traffic.rows(p.now)[0].expiresAtMs, until + SessionFiles::KEPT_MS);
    // Gone when its time is up.
    p.b.traffic.tick(until + 1000);
    CHECK_EQ(p.b.files.keptCount(), (size_t)1);
    CHECK(p.b.history.entries().empty());
    p.b.traffic.tick(until + SessionFiles::KEPT_MS);
    CHECK_EQ(p.b.files.keptCount(), (size_t)0);
    CHECK(!fs::exists(pathFromUtf8(scratch.path("k.bin") + ".part")));
    // Its file is done for now, and goes into Completed as it ended.
    CHECK(p.b.history.entries().size() == 1 && p.b.history.entries()[0].name == "K.BIN"
          && p.b.history.entries()[0].result == Result::Failed
          && p.b.history.entries()[0].atMs == until + SessionFiles::KEPT_MS);

    // Delete part.
    Pair q;
    q.perStep = 1000;
    q.a.files.offer("W1AW", scratch.write("L.BIN", 20000), q.now, error);
    q.connect();
    q.steps(3);
    q.b.files.accept(q.b.transfer(false)->id, scratch.path("l.bin"), error);
    q.steps(5);
    q.disconnect();
    rows = q.b.traffic.rows(q.now);
    CHECK(q.b.history.entries().empty());
    CHECK(rows.size() == 1 && q.b.files.deleteKept(rows[0].id));
    CHECK(q.b.traffic.rows(q.now).empty() && !fs::exists(pathFromUtf8(scratch.path("l.bin") + ".part")));
    q.b.traffic.tick(q.now);
    CHECK(q.b.history.entries().size() == 1 && q.b.history.entries()[0].name == "L.BIN");

    // A part restored whose file has gone is dropped.
    KeptPart gone;
    gone.peer = "W1AW";
    gone.size = 100;
    gone.bytes = 50;
    gone.part = scratch.path("nothing.part");
    gone.keptAtMs = q.now;
    SessionFiles fresh;
    fresh.restoreKept({gone}, q.now);
    CHECK_EQ(fresh.keptCount(), (size_t)0);
}

void testDeclinedAndExpired()
{
    Scratch scratch("declined");
    Pair p;
    std::string error;
    p.a.files.offer("W1AW", scratch.write("NO.TXT", 100), p.now, error);
    p.connect();
    p.steps(2);
    CHECK(p.b.files.decline(p.b.transfer(false)->id));
    p.steps(2);
    CHECK(p.a.transfer(true)->state == State::Declined);
    CHECK(!p.a.history.entries().empty() && resultLabel(p.a.history.entries().back()) == "Declined by W1AW");
    CHECK(!p.b.history.entries().empty() && resultLabel(p.b.history.entries().back()) == "Declined");

    p.a.files.offer("W1AW", scratch.write("LATE.TXT", 100), p.now, error);
    p.steps(2);
    p.steps(4 * 60 + 2);
    CHECK(p.a.transfer(true)->state == State::Expired);
    CHECK(p.b.transfer(false)->state == State::Expired);
    CHECK(resultLabel(p.a.history.entries().back()) == "Expired, no answer");
}

//-------------------------------------------------------------------------
// The tab's words, numbers and log
//-------------------------------------------------------------------------

void testWordsAndNumbers()
{
    CHECK_EQ(groupedNumber(0), std::string("0"));
    CHECK_EQ(groupedNumber(7000), std::string("7,000"));
    CHECK_EQ(groupedNumber(307200), std::string("307,200"));
    CHECK_EQ(groupedNumber(1234567), std::string("1,234,567"));
    CHECK_EQ(sizeText(1), std::string("1 byte"));
    CHECK_EQ(sizeText(7000), std::string("7,000 bytes"));
    CHECK_EQ(sizeText(307200), std::string("300.0 KB"));
    CHECK_EQ(progressText(4096, 7000), std::string("4,096 of 7,000"));
    CHECK_EQ(rateText(0), std::string());
    CHECK_EQ(rateText(4.25), std::string("4.2 B/s"));
    CHECK_EQ(rateText(38.4), std::string("38 B/s"));
    CHECK_EQ(rateText(1229), std::string("1.2 KB/s"));
    CHECK_EQ(peerText(true, "W1AW"), std::string("OM \xe2\x86\x92 W1AW"));
    CHECK_EQ(peerText(false, "W1AW"), std::string("W1AW \xe2\x86\x92 OM"));
    CHECK_EQ(peerText(true, BROADCAST_PEER), std::string("OM \xe2\x86\x92 Broadcast"));

    // 2026-10-22 14:19 UTC; local is UTC-7.
    uint64_t now = T0 + (14 * 60 + 19) * 60000ull;
    long pdt = -7 * 3600;
    CHECK_EQ(finishText(now + 13 * 60000, now, 0), std::string("ETA 14:32"));
    CHECK_EQ(finishText(now + 13 * 60000, now, pdt), std::string("ETA 07:32"));
    CHECK_EQ(finishText(now + 20000, now, 0), std::string("ETA 14:19"));
    CHECK_EQ(finishText(0, now, 0), std::string());
    CHECK_EQ(finishText(now + 3 * 86400000ull, now, 0), std::string("ETA Oct 25"));
    FileRow moving;
    moving.state = "QRL";
    moving.finishAtMs = now + 13 * 60000;
    CHECK_EQ(stateText(moving, now, 0), std::string("QRL ETA 14:32"));
    moving.state = "Receiving";
    CHECK_EQ(stateText(moving, now, 0), std::string("Receiving, ETA 14:32"));
    moving.finishAtMs = 0;
    CHECK_EQ(stateText(moving, now, 0), std::string("Receiving"));
    CHECK_EQ(expiryText(now + 190000, now, 0), std::string("3:10"));
    CHECK_EQ(expiryText(now + 83 * 60000, now, 0), std::string("15:42 (1 h 23 min)"));
    CHECK_EQ(expiryText(now + 7 * 86400000ull, now, 0), std::string("Oct 29 (7 days)"));
    CHECK_EQ(expiryText(0, now, 0), std::string());
    CHECK_EQ(dateText(now - 3600000, now, 0), std::string("13:19"));
    CHECK_EQ(dateText(now - 86400000, now, 0), std::string("Oct 21 14:19"));
    // Local time decides what today is.
    CHECK_EQ(dateText(T0 + 3600000, T0 + 7200000, pdt), std::string("18:00"));
    CHECK_EQ(dateText(T0 - 3600000, T0 + 7200000, pdt), std::string("16:00"));
    CHECK_EQ(dateText(T0 + 3600000, T0 + 9 * 3600000ull, pdt), std::string("Oct 21 18:00"));
}

void testRateMeter()
{
    RateMeter m;
    CHECK(m.rate() == 0);
    // 50 bytes a second for a minute.
    for (int s = 0; s <= 60; s++) m.sample((uint64_t)s * 50, T0 + (uint64_t)s * 1000);
    CHECK(m.rate() > 49.9 && m.rate() < 50.1);
    CHECK_EQ(m.finishAtMs(6000, T0 + 60000), T0 + 120000);
    // Then 10 a second: after two minutes of it only the new rate counts.
    for (int s = 61; s <= 200; s++) m.sample(3000 + (uint64_t)(s - 60) * 10, T0 + (uint64_t)s * 1000);
    CHECK(m.rate() > 9.9 && m.rate() < 10.1);
    // Standing still: the rate falls over the window, and lastMoved stays.
    uint64_t moved = m.lastMovedMs();
    for (int s = 201; s <= 260; s++) m.sample(4400, T0 + (uint64_t)s * 1000);
    CHECK_EQ(m.lastMovedMs(), moved);
    CHECK(m.rate() > 4.9 && m.rate() < 5.1);
    // A new start from further back.
    m.sample(100, T0 + 261000);
    CHECK(m.rate() == 0);
}

void testTheLogAsText()
{
    FileHistory history;
    FinishedFile f;
    f.via = Via::Session;
    f.atMs = T0;
    f.outgoing = false;
    f.peer = "W1AW";
    f.name = "tab\there.txt";
    f.size = 7000;
    f.result = Result::Failed;
    f.reason = "line one\nline two \\ backslash";
    f.path = "/home/op/Ragtime received/tab\there.txt";
    uint64_t first = history.add(f);
    f.via = Via::Broadcast;
    f.outgoing = true;
    f.peer = BROADCAST_PEER;
    f.name = "net.txt";
    f.result = Result::Sent;
    f.reason.clear();
    f.movedBytes = 6000;
    f.movedMs = 60000;
    history.add(f);
    // A picture the far end had enough of.
    f.via = Via::Session;
    f.peer = "KC2G";
    f.name = "shack.j2k";
    f.size = 20000;
    f.result = Result::Sent;
    f.picture = true;
    f.enough = 8192;
    history.add(f);
    CHECK(history.find(first) != nullptr);

    KeptPart k;
    k.peer = "KC2G";
    k.name = "map.png";
    k.size = 300000;
    for (int i = 0; i < 12; i++) k.digest[i] = (uint8_t)(i * 17 + 1);
    k.path = "/tmp/map.png";
    k.part = "/tmp/map.png.part";
    k.bytes = 4096;
    k.keptAtMs = T0;
    k.keepUntilMs = T0 + SessionFiles::KEPT_MS;

    std::string text = saveFilesText(history, {k}, {"B\tbroadcast's own line"});
    CHECK(text.compare(0, 16, "ragtime-files 1\n") == 0);
    FileHistory back;
    std::vector<KeptPart> kept;
    std::vector<std::string> other;
    CHECK(loadFilesText(text + "Z\tgarbage\nF\tnot\tenough\n", back, kept, &other));
    CHECK_EQ(back.entries().size(), (size_t)3);
    if (back.entries().size() == 3)
    {
        const FinishedFile& picture = back.entries()[2];
        CHECK(picture.picture && picture.enough == 8192 && !back.entries()[1].picture);
        CHECK_EQ(resultLabel(picture), std::string("Sent: KC2G had enough at 40%"));
        const FinishedFile& b = back.entries()[0];
        CHECK_EQ(b.name, std::string("tab\there.txt"));
        CHECK_EQ(b.reason, std::string("line one\nline two \\ backslash"));
        CHECK_EQ(b.path, std::string("/home/op/Ragtime received/tab\there.txt"));
        CHECK(b.result == Result::Failed && !b.outgoing && b.atMs == T0 && b.size == 7000);
        CHECK(back.entries()[1].via == Via::Broadcast && back.entries()[1].peer == "Broadcast");
        CHECK(b.movedBytes == 0 && bpsText(b).empty());
        CHECK(back.entries()[1].movedBytes == 6000 && back.entries()[1].movedMs == 60000);
        CHECK_EQ(bpsText(back.entries()[1]), std::string("800"));
    }
    // A line from before the bps column still loads, with no rate.
    FileHistory older;
    std::vector<KeptPart> none;
    CHECK(loadFilesText("ragtime-files 1\nF\tsession\t5\tin\tW1AW\ta.txt\t10\tsaved\t\t/tmp/a.txt\n", older, none));
    CHECK(older.entries().size() == 1 && older.entries()[0].movedMs == 0 && older.entries()[0].path == "/tmp/a.txt");

    // A log from before 2026-10-23 put a file in Completed as its part was
    // kept: the entry moves to the part, until the part goes.
    FileHistory tidied;
    std::vector<KeptPart> parts;
    CHECK(loadFilesText("ragtime-files 1\n"
                        "F\tsession\t100000\tin\tKC2G\teq.py\t23446\tcancelled-there\t\t\n"
                        "F\tsession\t5000\tin\tKC2G\teq.py\t23446\tsaved\t\t/tmp/eq.py\n"
                        "K\tKC2G\teq.py\t23446\t000102030405060708090a0b\t/tmp/eq.py\t/tmp/eq.py.part\t1476\t100400\t0\n",
                        tidied, parts));
    CHECK(tidied.entries().size() == 1 && tidied.entries()[0].result == Result::Saved);
    CHECK(parts.size() == 1 && parts[0].hasEnding && resultLabel(parts[0].ending) == "Cancelled by KC2G");
    CHECK(kept.size() == 1 && kept[0].bytes == 4096 && kept[0].digest[11] == (uint8_t)(11 * 17 + 1));
    CHECK(other.size() == 2 && other[0] == "B\tbroadcast's own line" && other[1] == "Z\tgarbage");
    CHECK(!loadFilesText("something else\n", back, kept));

    // A part sent: an S line, its file and its ending kept, and a fresh
    // engine takes it back with the far end's count as it was.
    KeptPart s = k;
    s.outgoing = true;
    s.part.clear();
    s.bytes = 1244;
    s.handed = 3741;
    s.hasEnding = true;
    s.ending.result = Result::Cancelled;
    std::string withSent = saveFilesText(FileHistory(), {s});
    CHECK(withSent.find("\nS\tKC2G\tmap.png\t") != std::string::npos);
    FileHistory noHistory;
    std::vector<KeptPart> sentBack;
    std::vector<std::string> notOurs;
    CHECK(loadFilesText(withSent, noHistory, sentBack, &notOurs) && notOurs.empty());
    CHECK(sentBack.size() == 1 && sentBack[0].outgoing && sentBack[0].path == "/tmp/map.png");
    if (sentBack.size() == 1)
    {
        CHECK(sentBack[0].bytes == 1244 && sentBack[0].handed == 1244 && sentBack[0].ending.outgoing);
        CHECK_EQ(partStateText(sentBack[0]), std::string("Part sent, cancelled"));
        SessionFiles engine;
        engine.restoreKept(sentBack, T0 + 1000);
        CHECK(engine.keptParts().size() == 1 && engine.keptParts()[0].outgoing && engine.keptCount() == 0);
        // Past its time, it goes to Completed as it ended.
        engine.tick(T0 + SessionFiles::KEPT_MS + 1);
        std::vector<FinishedFile> ended = engine.takeEnded();
        CHECK(engine.keptParts().empty() && ended.size() == 1 && ended[0].result == Result::Cancelled);
    }

    // The oldest go past the cap.
    FileHistory many;
    for (size_t i = 0; i < FileHistory::MAX_ENTRIES + 5; i++)
    {
        f.size = i;
        many.add(f);
    }
    CHECK_EQ(many.entries().size(), FileHistory::MAX_ENTRIES);
    CHECK_EQ(many.entries().front().size, (uint64_t)5);
    CHECK(many.remove(many.entries().front().id) && many.entries().front().size == 6);
}

} // namespace

int main()
{
    testAFileGoesAndIsSaved();
    testFileBytesArePaced();
    testTheWindowFollowsTheMode();
    testAFastModeKeepsTheHostFull();
    testACancelIsCut();
    testAMessageCutsIn();
    testLeavingCutsFirst();
    testAnOlderHostIsNotCut();
    testStalledWhileTheLinkFades();
    testAStationThatTakesNoFiles();
    testOffersWaitForTheHello();
    testLeavingKeepsThePartAndTheQueue();
    testKeptPartsMenu();
    testDeclinedAndExpired();
    testWordsAndNumbers();
    testRateMeter();
    testTheLogAsText();
    return ragtime_test::finish();
}
