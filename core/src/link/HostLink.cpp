#include "ragtime/link/HostLink.h"

#include <algorithm>
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
            buffer_ = c.count;
            Event e;
            e.type = Event::Type::Buffer;
            e.count = c.count;
            emit(e);
            break;
        }
        case Type::Cqframe:
        case Type::Other:
            break;
    }
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
                             uint64_t nowMs)
{
    if (frames.empty()) return 0;
    advance(nowMs);
    Burst& burst = bursts_.emplace_back();
    burst.id = nextId_++;
    burst.frames = frames;
    burst.mode = mode;
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

void HostLink::finishSession(SessionEnd end)
{
    Event e;
    e.type = Event::Type::SessionEnded;
    e.peer = peer_;
    e.sessionEnd = end;
    session_ = SessionState::None;
    buffer_ = 0;
    abortMs_ = 0;
    emit(e);
    heldChanged();
}

} // namespace link
} // namespace ragtime
