#include "ragtime/link/HostLink.h"

#include <algorithm>
#include <string>
#include <utility>

namespace ragtime
{
namespace link
{

namespace
{

constexpr size_t FINISHED_KEPT = 256;

const char* const OK = "OK";
const char* const BCAST_PORT = "BCAST PORT";
const char* const MODES = "MODES";
const char* const SN = "SN";
const char* const DATA2G = "DATA2G";
const char* const CLEAN = "CLEANTXBUFFER";

} // namespace

HostLink::HostLink(Settings settings) : settings_(std::move(settings))
{
}

//-------------------------------------------------------------------------
// The ports
//-------------------------------------------------------------------------

void HostLink::sendLine(const std::string& line, const std::string& answer, uint64_t nowMs)
{
    if (!commandUp_) return;
    commandOut_ += line + "\r";
    if (awaiting_.empty()) awaitingSinceMs_ = nowMs;
    awaiting_.push_back({line, answer});
}

void HostLink::commandUp(uint64_t nowMs)
{
    commandDown(nowMs);
    commandUp_ = true;
    lastMs_ = nowMs;
    std::string call = commandCallsign(settings_.callsign);
    sendLine("MYCALL " + call, OK, nowMs);
    sendLine("CHAT ON", OK, nowMs);
    sendLine("DATA2G ON", DATA2G, nowMs);
    sendLine(settings_.acceptSessions ? "LISTEN ON" : "LISTEN OFF", OK, nowMs);
    sendLine(settings_.narrow ? "BW500" : "BW2300", OK, nowMs);
    sendLine(std::string("BCAST OPEN ") + GROUP + " FROM " + call, BCAST_PORT, nowMs);
    sendLine("MODES", MODES, nowMs);
}

void HostLink::commandDown(uint64_t nowMs)
{
    (void)nowMs;
    bool wasUp = commandUp_;
    commandUp_ = false;
    commandOut_.clear();
    kissOut_.clear();
    lines_.reset();
    awaiting_.clear();
    modeAsked_ = false;
    modesComing_.clear();
    if (groupPort_ >= 0)
    {
        groupPort_ = -1;
        Event e;
        e.type = Event::Type::GroupClosed;
        emit(e);
    }
    groupMode_.clear();
    ptt_ = false;
    busy_ = false;
    data2g_ = false;
    lastMode_.clear();

    // A burst the host had may or may not have gone: it isn't Sent.
    Burst* burst = current();
    if (burst != nullptr && burst->state != BurstState::Queued)
    {
        setBurst(*burst, BurstState::NotSent);
        bursts_.pop_front();
    }
    if (wasUp && session_ != SessionState::None) finishSession(SessionEnd::HostLost);
}

void HostLink::commandBytes(const char* bytes, size_t length, uint64_t nowMs)
{
    advance(nowMs);
    std::vector<std::string> lines;
    lines_.feed(bytes, length, lines);
    for (const std::string& line : lines) onLine(line, nowMs);
    pump(nowMs);
}

std::string HostLink::takeCommandOutput()
{
    std::string out;
    out.swap(commandOut_);
    return out;
}

void HostLink::kissBytes(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    advance(nowMs);
    std::vector<KissFrame> frames;
    kiss_.feed(bytes, length, frames);
    for (KissFrame& frame : frames)
    {
        if (frame.command == KISS_ACKMODE)
        {
            if (frame.payload.size() < 2) continue;
            uint16_t tag = (uint16_t)((frame.payload[0] << 8) | frame.payload[1]);
            Burst* burst = current();
            if (burst == nullptr || burst->state == BurstState::Queued) continue;
            auto it = std::find(burst->tags.begin(), burst->tags.end(), tag);
            if (it == burst->tags.end()) continue;
            burst->tags.erase(it);
            if (burst->tags.empty())
            {
                setBurst(*burst, BurstState::Sent);
                bursts_.pop_front();
            }
        }
        else if (frame.port == groupPort_ && !frame.payload.empty())
        {
            Event e;
            e.type = Event::Type::Heard;
            e.frame = std::move(frame.payload);
            emit(std::move(e));
        }
    }
    pump(nowMs);
}

std::vector<uint8_t> HostLink::takeKissOutput()
{
    std::vector<uint8_t> out;
    out.swap(kissOut_);
    return out;
}

std::vector<HostLink::Event> HostLink::takeEvents()
{
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

// Counts the time a burst has been with the host while no session was up.
void HostLink::advance(uint64_t nowMs)
{
    uint64_t elapsed = nowMs > lastMs_ ? nowMs - lastMs_ : 0;
    lastMs_ = std::max(lastMs_, nowMs);
    Burst* burst = current();
    bool withHost = burst != nullptr && (burst->state == BurstState::Sending || burst->state == BurstState::Held);
    // A session holds the group; only time without one counts.
    if (withHost && session_ == SessionState::None) burst->unsentMs += elapsed;
}

void HostLink::tick(uint64_t nowMs)
{
    advance(nowMs);

    // A host that never answers a command isn't waited on for ever.
    if (!awaiting_.empty() && nowMs - awaitingSinceMs_ >= ANSWER_MS)
    {
        awaiting_.clear();
        if (modeAsked_) answered(true, nowMs);
    }

    // Leave now always leaves, whatever the host makes of the ABORT.
    if (session_ == SessionState::Leaving && abortMs_ != 0 && nowMs >= abortMs_ + ABORT_WAIT_MS)
        finishSession(SessionEnd::Left);

    askSn(nowMs);

    Burst* burst = current();
    bool withHost = burst != nullptr && (burst->state == BurstState::Sending || burst->state == BurstState::Held);
    if (withHost && burst->unsentMs >= NOT_SENT_MS)
    {
        setBurst(*burst, BurstState::NotSent);
        bursts_.pop_front();
    }
    pump(nowMs);
}

//-------------------------------------------------------------------------
// The host's lines
//-------------------------------------------------------------------------

void HostLink::answered(bool ok, uint64_t nowMs)
{
    (void)nowMs;
    if (!modeAsked_) return;
    modeAsked_ = false;
    Burst* burst = current();
    if (burst == nullptr) return;
    if (ok)
    {
        groupMode_ = burst->mode;
        writeBurst(*burst);
    }
    else
    {
        // The host won't send in that mode.
        setBurst(*burst, BurstState::NotSent);
        bursts_.pop_front();
    }
}

void HostLink::onLine(const std::string& line, uint64_t nowMs)
{
    CommandEvent c = parseCommandLine(line);
    using Type = CommandEvent::Type;

    // Answers to what we sent, in order.
    auto popAwaiting = [&]() {
        if (awaiting_.empty()) return Awaiting();
        Awaiting a = awaiting_.front();
        awaiting_.pop_front();
        awaitingSinceMs_ = nowMs;
        return a;
    };

    switch (c.type)
    {
        case Type::Ok:
        case Type::Wrong:
        {
            bool ok = c.type == Type::Ok;
            Awaiting a = popAwaiting();
            // No SNR yet, or a host from before Data2G PR #98: nothing refused.
            if (a.answer == SN) break;
            // A host from before Data2G PR #118: no cuts, nothing refused.
            if (a.answer == DATA2G)
            {
                data2g_ = ok;
                break;
            }
            if (a.answer == CLEAN)
            {
                data2g_ = data2g_ && ok;
                Event e;
                e.type = Event::Type::Clean;
                e.clean.result = CommandEvent::Clean::Result::Failed;
                emit(e);
                break;
            }
            if (a.answer == MODES && ok)
            {
                modes_ = modesComing_;
                modesComing_.clear();
                Event e;
                e.type = Event::Type::ModesChanged;
                emit(e);
            }
            if (!ok)
            {
                Event e;
                e.type = Event::Type::Refused;
                e.text = a.command;
                emit(e);
                if (a.answer == BCAST_PORT) break;
                // A call the host won't make.
                if (a.command.compare(0, 8, "CONNECT ") == 0 && session_ == SessionState::Calling)
                {
                    finishSession(SessionEnd::Refused);
                    break;
                }
            }
            if (a.command.compare(0, 10, "BCAST MODE") == 0) answered(ok, nowMs);
            break;
        }
        case Type::BcastPort:
        {
            if (!awaiting_.empty() && awaiting_.front().answer == BCAST_PORT) popAwaiting();
            groupPort_ = c.number;
            Event e;
            e.type = Event::Type::GroupOpen;
            e.number = c.number;
            emit(e);
            break;
        }
        case Type::ModeLine:
            if (!awaiting_.empty() && awaiting_.front().answer == MODES) modesComing_.push_back(c.modeInfo);
            break;
        case Type::Ptt:
        case Type::Busy:
        {
            bool& flag = c.type == Type::Ptt ? ptt_ : busy_;
            if (flag == c.on) break;
            flag = c.on;
            Event e;
            e.type = c.type == Type::Ptt ? Event::Type::Ptt : Event::Type::Busy;
            e.on = c.on;
            emit(e);
            break;
        }
        case Type::Mode:
        {
            lastMode_ = c.text;
            Event e;
            e.type = Event::Type::ModeUsed;
            e.text = c.text;
            emit(e);
            break;
        }
        case Type::BcastHeard:
        {
            if (c.number != groupPort_ || c.text.empty()) break;
            Event e;
            e.type = Event::Type::HeardFrom;
            e.text = c.text;
            e.snr = c.snr;
            emit(e);
            break;
        }
        case Type::BcastMissed:
        {
            // Any open port's, maybe another group's: only a hint.
            if (groupPort_ < 0) break;
            Event e;
            e.type = Event::Type::Missed;
            e.text = c.text;
            e.count = c.count;
            e.snr = c.snr;
            emit(e);
            break;
        }
        case Type::Sn:
        {
            if (!awaiting_.empty() && awaiting_.front().answer == SN) popAwaiting();
            // The same figure again is the same burst, asked twice.
            if (session_ == SessionState::None || session_ == SessionState::Calling || c.snr == sessionSnr_) break;
            sessionSnr_ = c.snr;
            Event e;
            e.type = Event::Type::SessionSnr;
            e.peer = peer_;
            e.snr = c.snr;
            emit(e);
            break;
        }
        case Type::BcastLost:
        {
            if (c.number != groupPort_) break;
            Event e;
            e.type = Event::Type::GroupLost;
            e.count = c.count;
            emit(e);
            break;
        }
        case Type::BcastDropped:
        {
            Burst* burst = current();
            if (c.number != groupPort_ || burst == nullptr || burst->state == BurstState::Queued) break;
            setBurst(*burst, BurstState::NotSent);
            bursts_.pop_front();
            break;
        }
        case Type::Connected:
        {
            std::string me = commandCallsign(settings_.callsign);
            incoming_ = c.text != me;
            peer_ = incoming_ ? c.text : c.peer;
            session_ = SessionState::Connected;
            buffer_ = 0;
            snWanted_ = false;
            snAskedMs_ = 0;
            sessionSnr_.reset();
            Event e;
            e.type = Event::Type::SessionConnected;
            e.peer = peer_;
            e.incoming = incoming_;
            e.number = c.number;
            emit(e);
            heldChanged();
            break;
        }
        case Type::Disconnected:
            if (session_ == SessionState::Calling) finishSession(SessionEnd::NoAnswer);
            else if (session_ == SessionState::Leaving) finishSession(SessionEnd::Left);
            else if (session_ == SessionState::Connected) finishSession(SessionEnd::Ended);
            break;
        case Type::Buffer:
        {
            // After DISCONNECTED, BUFFER 0 is the host listening again.
            if (session_ == SessionState::None) break;
            // Fewer bytes waiting: the partner's acknowledgement came in a burst.
            if (c.count < buffer_) snWanted_ = true;
            buffer_ = c.count;
            askSn(nowMs);
            Event e;
            e.type = Event::Type::Buffer;
            e.count = c.count;
            emit(e);
            break;
        }
        case Type::Clean:
        {
            // SETTLED comes by itself; the rest answer CLEANTXBUFFER.
            if (c.clean.result != CommandEvent::Clean::Result::Settled && !awaiting_.empty()
                && awaiting_.front().answer == CLEAN)
                popAwaiting();
            Event e;
            e.type = Event::Type::Clean;
            e.clean = c.clean;
            emit(e);
            break;
        }
        case Type::Cqframe:
        case Type::Other:
            break;
    }
}

bool HostLink::cleanTxBuffer(uint64_t upto, uint64_t nowMs)
{
    if (!canCut()) return false;
    sendLine("CLEANTXBUFFER " + std::to_string(upto), CLEAN, nowMs);
    return true;
}

//-------------------------------------------------------------------------
// Settings
//-------------------------------------------------------------------------

void HostLink::setAcceptSessions(bool accept, uint64_t nowMs)
{
    if (settings_.acceptSessions == accept) return;
    settings_.acceptSessions = accept;
    sendLine(accept ? "LISTEN ON" : "LISTEN OFF", OK, nowMs);
}

void HostLink::setNarrow(bool narrow, uint64_t nowMs)
{
    if (settings_.narrow == narrow) return;
    settings_.narrow = narrow;
    // BW500 caps the sessions the host opens and narrows its MODES.
    sendLine(narrow ? "BW500" : "BW2300", OK, nowMs);
    sendLine("MODES", MODES, nowMs);
}

//-------------------------------------------------------------------------
// Broadcasts
//-------------------------------------------------------------------------

HostLink::Burst* HostLink::current()
{
    return bursts_.empty() ? nullptr : &bursts_.front();
}

uint64_t HostLink::broadcast(const std::vector<std::vector<uint8_t>>& frames, const std::string& mode,
                             uint64_t nowMs, const std::string& label)
{
    if (frames.empty()) return 0;
    advance(nowMs);
    Burst& burst = bursts_.emplace_back();
    burst.id = nextId_++;
    burst.frames = frames;
    burst.mode = mode;
    burst.label = label;
    Event e;
    e.type = Event::Type::Burst;
    e.id = burst.id;
    e.burstState = BurstState::Queued;
    emit(e);
    uint64_t id = burst.id;
    pump(nowMs);
    return id;
}

bool HostLink::cancel(uint64_t id)
{
    for (auto it = bursts_.begin(); it != bursts_.end(); ++it)
    {
        if (it->id != id) continue;
        // Asked for its mode, it is still ours; written, it is the host's.
        if (it->state != BurstState::Queued) return false;
        if (it == bursts_.begin()) modeAsked_ = false;
        setBurst(*it, BurstState::NotSent);
        bursts_.erase(it);
        return true;
    }
    return false;
}

HostLink::BurstState HostLink::burstState(uint64_t id) const
{
    for (const Burst& burst : bursts_)
    {
        if (burst.id == id) return burst.state;
    }
    for (const auto& f : finished_)
    {
        if (f.first == id) return f.second;
    }
    return BurstState::NotSent;
}

bool HostLink::burstWithHost() const
{
    if (bursts_.empty()) return false;
    BurstState state = bursts_.front().state;
    return state == BurstState::Sending || state == BurstState::Held;
}

std::vector<HostLink::Outgoing> HostLink::outgoing() const
{
    std::vector<Outgoing> out;
    for (const Burst& burst : bursts_)
    {
        Outgoing o;
        o.id = burst.id;
        o.label = burst.label;
        o.state = burst.state;
        o.frames = burst.frames.size();
        out.push_back(std::move(o));
    }
    return out;
}

void HostLink::setPaused(bool paused, uint64_t nowMs)
{
    advance(nowMs);
    paused_ = paused;
    pump(nowMs);
}

void HostLink::setBurst(Burst& burst, BurstState state)
{
    if (burst.state == state) return;
    burst.state = state;
    if (state == BurstState::Sent || state == BurstState::NotSent)
    {
        finished_.push_back({burst.id, state});
        if (finished_.size() > FINISHED_KEPT) finished_.erase(finished_.begin());
    }
    Event e;
    e.type = Event::Type::Burst;
    e.id = burst.id;
    e.burstState = state;
    emit(e);
}

void HostLink::writeBurst(Burst& burst)
{
    for (const std::vector<uint8_t>& frame : burst.frames)
    {
        uint16_t tag = nextTag_++;
        if (nextTag_ == 0) nextTag_ = 1;
        std::vector<uint8_t> kiss = kissEncodeAckMode(groupPort_, tag, frame);
        kissOut_.insert(kissOut_.end(), kiss.begin(), kiss.end());
        burst.tags.push_back(tag);
    }
    setBurst(burst, session_ == SessionState::None ? BurstState::Sending : BurstState::Held);
}

// Hands the host the next burst, once it has none of ours.
void HostLink::pump(uint64_t nowMs)
{
    Burst* burst = current();
    if (burst == nullptr || burst->state != BurstState::Queued || modeAsked_) return;
    if (!commandUp_ || groupPort_ < 0 || paused_) return;
    if (!burst->mode.empty() && burst->mode != groupMode_)
    {
        // The answer must be the mode's, so ask once the host has answered
        // everything before it.
        if (!awaiting_.empty()) return;
        modeAsked_ = true;
        sendLine("BCAST MODE " + std::to_string(groupPort_) + " " + burst->mode, OK, nowMs);
        return;
    }
    writeBurst(*burst);
}

void HostLink::heldChanged()
{
    Burst* burst = current();
    if (burst == nullptr) return;
    if (burst->state == BurstState::Sending && session_ != SessionState::None) setBurst(*burst, BurstState::Held);
    else if (burst->state == BurstState::Held && session_ == SessionState::None) setBurst(*burst, BurstState::Sending);
}

//-------------------------------------------------------------------------
// Sessions
//-------------------------------------------------------------------------

bool HostLink::call(const std::string& peer, uint64_t nowMs)
{
    std::string them = commandCallsign(peer);
    if (!commandUp_ || session_ != SessionState::None || them.empty()) return false;
    advance(nowMs);
    session_ = SessionState::Calling;
    peer_ = them;
    incoming_ = false;
    buffer_ = 0;
    abortMs_ = 0;
    sendLine("CONNECT " + commandCallsign(settings_.callsign) + " " + them, OK, nowMs);
    Event e;
    e.type = Event::Type::SessionCalling;
    e.peer = them;
    emit(e);
    heldChanged();
    return true;
}

bool HostLink::leave(uint64_t nowMs)
{
    if (session_ == SessionState::None || session_ == SessionState::Leaving) return false;
    if (session_ == SessionState::Calling) return leaveNow(nowMs);
    session_ = SessionState::Leaving;
    sendLine("DISCONNECT", OK, nowMs);
    return true;
}

bool HostLink::leaveNow(uint64_t nowMs)
{
    if (session_ == SessionState::None) return false;
    session_ = SessionState::Leaving;
    if (abortMs_ == 0) abortMs_ = std::max<uint64_t>(nowMs, 1);
    sendLine("ABORT", OK, nowMs);
    return true;
}

void HostLink::sessionHeard(uint64_t nowMs)
{
    if (session_ != SessionState::Connected && session_ != SessionState::Leaving) return;
    snWanted_ = true;
    askSn(nowMs);
}

void HostLink::askSn(uint64_t nowMs)
{
    if (!snWanted_ || !commandUp_) return;
    if (session_ != SessionState::Connected && session_ != SessionState::Leaving) return;
    if (snAskedMs_ != 0 && nowMs < snAskedMs_ + SN_GAP_MS) return;
    if (std::any_of(awaiting_.begin(), awaiting_.end(), [](const Awaiting& a) { return a.answer == SN; })) return;
    snWanted_ = false;
    snAskedMs_ = nowMs;
    sendLine(SN, SN, nowMs);
}

void HostLink::finishSession(SessionEnd end)
{
    Event e;
    e.type = Event::Type::SessionEnded;
    e.peer = peer_;
    e.sessionEnd = end;
    session_ = SessionState::None;
    buffer_ = 0;
    abortMs_ = 0;
    snWanted_ = false;
    sessionSnr_.reset();
    emit(e);
    heldChanged();
}

//-------------------------------------------------------------------------
// What is going out
//-------------------------------------------------------------------------

std::string snippet(const std::string& text, size_t chars)
{
    // On one line, its spaces run together.
    std::string flat;
    for (char c : text)
    {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (flat.empty() || flat.back() == ' ')) continue;
        flat += c;
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();

    // Where the chars-th character ends, counting UTF-8 lead bytes.
    size_t at = 0, count = 0;
    while (at < flat.size() && count < chars)
    {
        at++;
        while (at < flat.size() && ((unsigned char)flat[at] & 0xc0) == 0x80) at++;
        count++;
    }
    if (at >= flat.size()) return "\xe2\x80\x9c" + flat + "\xe2\x80\x9d";
    // Between words, if there is a space in the second half.
    size_t space = flat.rfind(' ', at);
    if (space != std::string::npos && space > at / 2) at = space;
    return "\xe2\x80\x9c" + flat.substr(0, at) + "\xe2\x80\xa6\xe2\x80\x9d";
}

namespace
{

// Short for the field, in full for its tooltip.
std::string described(const HostLink::Outgoing& o, const HostLink& link, bool full)
{
    const std::string what = o.label.empty() ? std::string("a burst") : o.label;
    switch (o.state)
    {
        case HostLink::BurstState::Sending:
            if (!link.busy()) return "Sending: " + what;
            return (full ? "Waiting for a clear channel: " : "Busy, next: ") + what;
        case HostLink::BurstState::Held:
            return (full ? "Held while the session is busy: " : "Held: ") + what;
        default:
            return (link.paused() ? "Paused: " : "Queued: ") + what;
    }
}

} // namespace

std::string channelLine(const HostLink& link)
{
    std::vector<HostLink::Outgoing> out = link.outgoing();
    if (out.empty()) return link.busy() ? "Channel busy" : "Channel clear";
    std::string line = described(out.front(), link, false);
    if (out.size() > 1) line += " (+" + std::to_string(out.size() - 1) + ")";
    return line;
}

std::string channelTip(const HostLink& link)
{
    std::string tip = link.busy() ? "Channel busy: data2g-host hears a signal" : "Channel clear";
    std::vector<HostLink::Outgoing> out = link.outgoing();
    if (out.empty()) tip += "\nNothing of ours waits to go on D2G";
    for (const HostLink::Outgoing& o : out) tip += "\n" + described(o, link, true);
    if (link.sessionState() != HostLink::SessionState::None && link.buffer() > 0)
        tip += "\nSession: " + std::to_string(link.buffer()) + " byte" + (link.buffer() == 1 ? "" : "s")
               + " not yet acknowledged by " + link.sessionPeer();
    return tip;
}

} // namespace link
} // namespace ragtime
