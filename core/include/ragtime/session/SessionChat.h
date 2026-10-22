// The Session tab's engine: one ragchew at a time, entered on purpose and
// left on purpose (docs/PLAN.md, "Sometimes we ragchew"; docs/PROTOCOL.md,
// "Sessions").
//
// A session starts only when the operator presses Call or one comes in,
// and ends only on Leave, Leave now, the far end leaving, or the host
// giving the link up. Nothing else here opens or closes one, and a quiet
// link shows as quiet, not closed.
//
// - Call: CONNECT through HostLink. The host calls up to five times; each
//   call keys the transmitter, so the host's PTT reports count the tries
//   ("Calling W1AW, try 2 of 5"). A host that never answers is given up
//   on after CALL_GIVE_UP_MS.
// - Connected: a fresh Numbering, and 0xA0 and our Hello on the data
//   port. The far end's stream is read with protocol::StreamReader, ready
//   from the end of the last session, since its Hello can come before
//   CONNECTED does.
// - Messages: typed text becomes a Text record in the stream. It is Sent
//   once written, and Delivered once the host's BUFFER count shows the far
//   end's modem has acknowledged its last byte (StreamAccount, below).
// - Leave: a Status QRV goes into the stream first, so the far end can say
//   "Session ended by AG7EW" rather than "Link lost"; once the host has
//   read it, DISCONNECT, which waits for what is unacknowledged. Leave now
//   is ABORT.
//
// Files (package G) and anything else that rides in a session plug in as
// a SessionTraffic. No sockets and no clock: the app hands in the data
// port's bytes and HostLink's events, and writes what this gives back.
#ifndef RAGTIME_SESSION_SESSION_CHAT_H
#define RAGTIME_SESSION_SESSION_CHAT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "ragtime/link/HostLink.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"

namespace ragtime
{
namespace session
{

// How much of our session stream the host has read and the far end's
// modem has acknowledged, from the host's BUFFER reports. BUFFER n is the
// bytes the host has read from us that the far end has not yet
// acknowledged. Every figure here is a lower bound, so nothing is ever
// counted delivered before it is:
//
// - A count that rose by k means the host read at least k more bytes
//   (acknowledgements only lower it).
// - Of what the host has read, all but n is acknowledged.
// - BUFFER 0 says everything read is acknowledged. One that comes
//   SETTLE_MS or more after a write settles that write as well: the host
//   reads its data port as bytes come and answers each read with a count,
//   so a 0 that late is not from before it read them. (One read that
//   takes an acknowledgement bigger than itself in the same step lowers
//   the count instead of raising it; this is what settles it.)
class StreamAccount
{
public:
    static constexpr uint64_t SETTLE_MS = 2000;

    // Our bytes have gone to the data port.
    void wrote(size_t bytes, uint64_t nowMs);
    // The host's BUFFER report.
    void buffer(int count, uint64_t nowMs);
    void reset();

    uint64_t written() const { return written_; }
    uint64_t read() const { return read_; }
    uint64_t acknowledged() const { return acked_; }
    // The host's latest count: bytes waiting for the far end.
    int waiting() const { return count_; }

private:
    struct Write
    {
        uint64_t end;
        uint64_t atMs;
    };
    std::deque<Write> writes_;      // not yet settled
    uint64_t written_ = 0;
    uint64_t read_ = 0;
    uint64_t acked_ = 0;
    int count_ = 0;
};

// One message in the Session tab.
struct Message
{
    enum class State
    {
        Heard,          // the far end's
        Queued,         // ours, not yet written (Pause sending)
        Sent,           // written into the session
        Delivered,      // the far end's modem has acknowledged it
        NotDelivered,   // the session ended first
    };

    uint64_t id = 0;
    bool ours = false;
    std::string from;
    std::string text;           // the exact UTF-8
    uint32_t seq = 0;
    uint32_t time = 0;          // the record's, Unix seconds
    uint64_t atMs = 0;          // when we made or heard it
    State state = State::Heard;
    uint64_t streamEnd = 0;     // ours, once written: where its record ends in our stream
};

// "Queued", "Sent", "Delivered", "Not delivered"; "" for one heard.
const char* stateLabel(Message::State state);

// What else rides in a session beside its messages: session files
// (package G). Records SessionChat doesn't take itself go to it, and it
// writes its own records into the stream. Every call names the peer.
class SessionTraffic
{
public:
    virtual ~SessionTraffic() = default;

    // The far end's Hello has come: what it takes is in takes.
    virtual void greeted(const std::string& peer, const protocol::Hello& hello) = 0;
    // A record from the far end that isn't Hello, Status or Text.
    virtual void onRecord(const std::string& peer, const protocol::Record& record, uint64_t nowMs) = 0;
    // Of a FileData still arriving, this much of the file is in.
    virtual void arriving(const std::string& peer, uint32_t offer, size_t bytes) = 0;
    // Records to write now, appended to out, numbered by the session's
    // numbering; streamOffset is where out's first byte will sit in our
    // stream. Called whenever SessionChat writes, and on every tick.
    virtual void write(const std::string& peer, protocol::Numbering& numbering, uint64_t streamOffset,
                       uint64_t nowMs, std::vector<uint8_t>& out) = 0;
    // The far end's modem has acknowledged our stream up to here.
    virtual void acknowledged(const std::string& peer, uint64_t streamAcked) = 0;
    // We are leaving: the last records to write before the farewell.
    virtual void leaving(const std::string& peer, protocol::Numbering& numbering, uint64_t nowMs,
                         std::vector<uint8_t>& out) = 0;
    virtual void ended(const std::string& peer, uint64_t nowMs) = 0;
    // Something is still moving with the peer: Leave asks first.
    virtual bool busy(const std::string& peer) const = 0;
};

class SessionChat
{
public:
    enum class Phase
    {
        None,           // no session yet
        Calling,
        Connected,
        Leaving,        // Leave or Leave now pressed
        Ended,          // the last one has ended; its messages stay readable
    };

    // Why the last session ended.
    enum class Ending
    {
        NoAnswer,       // five calls, or CALL_GIVE_UP_MS, with no answer
        Refused,        // the host wouldn't make the call
        Cancelled,      // we stopped the call
        Left,           // we left
        LeftThere,      // the far end left: its farewell came
        LinkLost,       // the host ended it, with no farewell from the far end
        HostLost,       // the command port closed
        DataLost,       // the session data port closed
    };

    struct Event
    {
        enum class Type
        {
            Calling,        // peer
            Started,        // peer, incoming: the tab pops open
            Ended,          // peer, ending
            Heard,          // a message from the far end: id
        } type = Type::Started;
        std::string peer;
        bool incoming = false;
        Ending ending = Ending::LinkLost;
        uint64_t id = 0;
    };

    struct Settings
    {
        std::string callsign;
        std::string locator;
        bool acceptsSessions = true;
        std::string software;       // "Ragtime 0.1.0"
    };

    // The host calls five times.
    static constexpr int CALL_TRIES = 5;
    // A call the host hasn't settled in this long is stopped (a host that
    // never answers a CONNECT).
    static constexpr uint64_t CALL_GIVE_UP_MS = 180 * 1000;
    // Our bytes waiting this long with nothing back from the far end: the
    // strip says "Link quiet".
    static constexpr uint64_t QUIET_MS = 30 * 1000;
    // Leave waits this long for the host to read the farewell before it
    // sends DISCONNECT anyway.
    static constexpr uint64_t FAREWELL_WAIT_MS = 10 * 1000;
    // The far end's farewell and the host's DISCONNECTED come on different
    // ports, in either order: for this long after a session ends without
    // one, the far end's stream is still read, and a farewell found there
    // turns "Link lost" into "Session ended by W1AW".
    static constexpr uint64_t AFTERGLOW_MS = 1000;
    // Messages kept in the tab; the oldest go first.
    static constexpr size_t MAX_MESSAGES = 2000;
    // The longest message taken at once, in bytes.
    static constexpr size_t MAX_TYPED = 2000;

    explicit SessionChat(link::HostLink& link);

    void setSettings(const Settings& settings);
    const Settings& settings() const { return settings_; }
    void setTraffic(SessionTraffic* traffic) { traffic_ = traffic; }

    //---------------------------------------------------------------------
    // The operator
    //---------------------------------------------------------------------

    // Call: false if the host or the data port isn't up, or a session is.
    bool call(const std::string& peer, uint64_t nowMs);
    // Leave: the farewell, then DISCONNECT. While calling, stops the call.
    bool leave(uint64_t nowMs);
    // Leave now: ABORT.
    bool leaveNow(uint64_t nowMs);
    // Sends typed text into the session; 0 for blank text, text over
    // MAX_TYPED, or no session up.
    uint64_t send(const std::string& typed, uint64_t nowMs);
    // Re-send: the text of one of our messages goes again as a new message,
    // in this session or a later one (the stream already resends what the
    // link loses, so the same record again would only be a duplicate). 0
    // for a message still Queued, or no session up.
    uint64_t resend(uint64_t id, uint64_t nowMs);
    static bool canResend(const Message& message) { return message.ours && message.state != Message::State::Queued; }
    // Pause sending: new messages wait as Queued.
    void setPaused(bool paused, uint64_t nowMs);
    bool paused() const { return paused_; }

    //---------------------------------------------------------------------
    // The ports
    //---------------------------------------------------------------------

    // Every HostLink event goes here.
    void onEvent(const link::HostLink::Event& event, uint64_t nowMs);
    // The data port's bytes. Returns the far end's records read, for the
    // station list; none before the session is up (they wait for it).
    std::vector<protocol::Record> dataBytes(const uint8_t* bytes, size_t length, uint64_t nowMs);
    // What to write to the data port.
    std::vector<uint8_t> takeDataOutput(uint64_t nowMs);
    // The data port connected or closed. Closed under a session, the
    // session ends: its BUFFER counts can't be matched to anything more.
    void dataPort(bool up, uint64_t nowMs);
    bool dataUp() const { return dataUp_; }
    // The timers: the call's give-up, the farewell's wait.
    void tick(uint64_t nowMs);

    std::vector<Event> takeEvents();

    //---------------------------------------------------------------------
    // What the tab shows
    //---------------------------------------------------------------------

    Phase phase() const { return phase_; }
    // A session is up or being called: the tab is live.
    bool active() const { return phase_ == Phase::Calling || phase_ == Phase::Connected || phase_ == Phase::Leaving; }
    const std::string& peer() const { return peer_; }
    bool incoming() const { return incoming_; }
    // "try 2 of 5": 1 to CALL_TRIES.
    int callTry() const;
    // When the phase began: the call, the connection, the leaving, the end.
    uint64_t sinceMs() const { return sinceMs_; }
    uint64_t connectedMs() const { return connectedMs_; }
    uint64_t lastHeardMs() const { return lastHeardMs_; }
    // Our bytes wait and nothing has come back for QUIET_MS.
    bool quiet(uint64_t nowMs) const;
    // Since anything last came back, while bytes wait.
    uint64_t quietForMs(uint64_t nowMs) const;
    int bandwidthHz() const { return bandwidthHz_; }
    const std::string& mode() const { return mode_; }
    Ending ending() const { return ending_; }
    // The far end's stream isn't Ragtime's.
    bool foreign() const { return reader_.foreign(); }
    bool greeted() const { return reader_.greeted(); }
    const protocol::Hello& theirHello() const { return reader_.hello(); }
    // Something rides in the session (files): Leave should ask first.
    bool trafficBusy() const;

    const StreamAccount& stream() const { return account_; }
    const protocol::Numbering& numbering() const { return numbering_; }

    const std::deque<Message>& messages() const { return messages_; }
    const Message* find(uint64_t id) const;
    // Goes up with every change, so a view knows when to redraw.
    uint64_t version() const { return version_; }

    // The status strip: "Calling W1AW, try 2 of 5", "Connected to W1AW ·
    // 14 min · 2300 Hz · 312 bytes waiting · heard 12 s ago", "Link quiet ·
    // 1:10 · data2g-host is still retrying", "Leaving, waiting for 312
    // bytes to be acknowledged", "Session ended by W1AW at 14:32".
    struct Strip
    {
        std::string text;
        bool amber = false;     // Link quiet
        bool live = false;      // a session is up or being called
    };
    // utcOffsetS turns the clock times into local time.
    Strip strip(uint64_t nowMs, long utcOffsetS = 0) const;

private:
    void begin(const std::string& peer, bool incoming, int bandwidth, uint64_t nowMs);
    void finish(Ending ending, uint64_t nowMs);
    void write(const std::vector<uint8_t>& bytes, uint64_t nowMs);
    void writeQueued(uint64_t nowMs);
    void onRecord(const protocol::Record& record, uint64_t nowMs);
    void settle();
    void checkFarewell(uint64_t nowMs);
    void heard(uint64_t nowMs);
    void trim();
    void endAfterglow();
    void emit(Event event) { events_.push_back(std::move(event)); }
    void changed() { version_++; }

    link::HostLink& link_;
    Settings settings_;
    SessionTraffic* traffic_ = nullptr;

    Phase phase_ = Phase::None;
    std::string peer_;
    bool incoming_ = false;
    int tries_ = 0;
    bool pttOn_ = false;
    uint64_t sinceMs_ = 0;
    uint64_t connectedMs_ = 0;
    uint64_t lastHeardMs_ = 0;
    uint64_t waitingSinceMs_ = 0;   // the latest time something came back, or our bytes began to wait
    int bandwidthHz_ = 0;
    std::string mode_;
    Ending ending_ = Ending::LinkLost;
    bool cancelling_ = false;       // we stopped the call
    bool gaveUp_ = false;           // CALL_GIVE_UP_MS ran out
    bool dataLost_ = false;
    bool farewellHeard_ = false;
    bool greetedTold_ = false;
    bool farewellPending_ = false;  // written; DISCONNECT once the host has read it
    uint64_t farewellEnd_ = 0;
    uint64_t farewellMs_ = 0;
    uint64_t afterglowUntilMs_ = 0;  // ended without a farewell: still reading until then
    bool dataUp_ = false;
    bool paused_ = false;

    protocol::Numbering numbering_;
    protocol::StreamReader reader_;
    std::vector<protocol::Record> early_;   // the far end's, read before the session was up
    StreamAccount account_;
    std::vector<uint8_t> out_;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> queued_;  // messages paused, by id, coded

    std::deque<Message> messages_;
    std::vector<Event> events_;
    uint64_t nextId_ = 1;
    uint64_t version_ = 0;
};

// "14 min", "1 h 5 min", "less than a minute".
std::string sessionLength(uint64_t ms);
// "1:10", "12:05", "1:02:03".
std::string clockLength(uint64_t ms);
// "14:32", in the offset's time.
std::string clockTime(uint64_t unixMs, long utcOffsetS);

} // namespace session
} // namespace ragtime

#endif
