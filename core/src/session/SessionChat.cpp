#include "ragtime/session/SessionChat.h"

#include <algorithm>
#include <utility>

#include "ragtime/broadcast/Stations.h"

namespace ragtime
{
namespace session
{

namespace
{

using LinkEvent = link::HostLink::Event;
using State = Message::State;

const char* const DOT = " \xc2\xb7 ";    // " · "

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

bool blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

std::string twoDigits(uint64_t n)
{
    return (n < 10 ? "0" : "") + std::to_string(n);
}

} // namespace

//-------------------------------------------------------------------------
// The stream's account
//-------------------------------------------------------------------------

void StreamAccount::wrote(size_t bytes, uint64_t nowMs)
{
    if (bytes == 0) return;
    written_ += bytes;
    writes_.push_back({written_, nowMs});
}

void StreamAccount::buffer(int count, uint64_t nowMs)
{
    if (count < 0) count = 0;
    // A rise is reading: acknowledgements only lower the count.
    if (count > count_) read_ = std::min(written_, read_ + (uint64_t)(count - count_));
    count_ = count;
    if (count == 0)
    {
        acked_ = std::max(acked_, read_);
        // A 0 this long after a write is from after the host read it.
        for (const Write& w : writes_)
        {
            if (w.atMs + SETTLE_MS <= nowMs) acked_ = std::max(acked_, w.end);
        }
        read_ = std::max(read_, acked_);
    }
    else if (read_ >= (uint64_t)count)
    {
        acked_ = std::max(acked_, read_ - (uint64_t)count);
    }
    while (!writes_.empty() && writes_.front().end <= acked_) writes_.pop_front();
}

void StreamAccount::reset()
{
    *this = StreamAccount();
}

//-------------------------------------------------------------------------

const char* stateLabel(Message::State state)
{
    switch (state)
    {
        case State::Heard:
            return "";
        case State::Queued:
            return "Queued";
        case State::Sent:
            return "Sent";
        case State::Delivered:
            return "Delivered";
        case State::NotDelivered:
            return "Not delivered";
    }
    return "";
}

std::string sessionLength(uint64_t ms)
{
    uint64_t min = ms / 60000;
    if (min == 0) return "less than a minute";
    if (min < 60) return std::to_string(min) + " min";
    std::string out = std::to_string(min / 60) + " h";
    if (min % 60 != 0) out += " " + std::to_string(min % 60) + " min";
    return out;
}

std::string clockLength(uint64_t ms)
{
    uint64_t s = ms / 1000;
    if (s < 3600) return std::to_string(s / 60) + ":" + twoDigits(s % 60);
    return std::to_string(s / 3600) + ":" + twoDigits(s / 60 % 60) + ":" + twoDigits(s % 60);
}

std::string clockTime(uint64_t unixMs, long utcOffsetS)
{
    long long day = 86400;
    long long t = (long long)(unixMs / 1000) + utcOffsetS;
    long long inDay = ((t % day) + day) % day;
    return twoDigits((uint64_t)(inDay / 3600)) + ":" + twoDigits((uint64_t)(inDay / 60 % 60));
}

SessionChat::SessionChat(link::HostLink& link) : link_(link)
{
}

void SessionChat::setSettings(const Settings& settings)
{
    settings_ = settings;
    settings_.callsign = protocol::normalCallsign(settings.callsign);
    changed();
}

//-------------------------------------------------------------------------
// The operator
//-------------------------------------------------------------------------

bool SessionChat::call(const std::string& peer, uint64_t nowMs)
{
    std::string them = protocol::normalCallsign(peer);
    if (active() || !dataUp_ || them.empty() || them == settings_.callsign) return false;
    if (!link_.call(them, nowMs)) return false;
    endAfterglow();
    phase_ = Phase::Calling;
    peer_ = them;
    incoming_ = false;
    tries_ = 0;
    sinceMs_ = nowMs;
    connectedMs_ = 0;
    cancelling_ = gaveUp_ = dataLost_ = false;
    // A new session starts in the tab: the last one's messages go.
    messages_.clear();
    SessionChat::Event calling;
    calling.type = SessionChat::Event::Type::Calling;
    calling.peer = them;
    emit(calling);
    changed();
    return true;
}

bool SessionChat::leave(uint64_t nowMs)
{
    if (phase_ == Phase::Calling) return leaveNow(nowMs);
    if (phase_ != Phase::Connected) return false;
    phase_ = Phase::Leaving;
    sinceMs_ = nowMs;
    if (dataUp_)
    {
        // The last word of whatever rides in the session, then the
        // farewell: our QRV, so the far end knows we left on purpose.
        std::vector<uint8_t> out;
        if (traffic_ != nullptr) traffic_->leaving(peer_, numbering_, nowMs, out);
        protocol::Status qrv;
        qrv.state = protocol::Status::State::Qrv;
        protocol::encodeRecord(numbering_.record(qrv, seconds(nowMs)), out);
        write(out, nowMs);
        farewellPending_ = true;
        farewellEnd_ = account_.written() + out_.size();
        farewellMs_ = nowMs;
    }
    else
    {
        link_.leave(nowMs);
    }
    changed();
    return true;
}

bool SessionChat::leaveNow(uint64_t nowMs)
{
    if (!active()) return false;
    if (phase_ == Phase::Calling) cancelling_ = true;
    farewellPending_ = false;
    phase_ = Phase::Leaving;
    sinceMs_ = nowMs;
    link_.leaveNow(nowMs);
    changed();
    return true;
}

uint64_t SessionChat::send(const std::string& typed, uint64_t nowMs)
{
    if (phase_ != Phase::Connected || settings_.callsign.empty() || blank(typed) || typed.size() > MAX_TYPED)
        return 0;
    protocol::Text t;
    t.text = typed;
    protocol::Record record = numbering_.record(t, seconds(nowMs));
    std::vector<uint8_t> bytes = protocol::encodeRecord(record);
    if (bytes.empty()) return 0;

    Message m;
    m.id = nextId_++;
    m.ours = true;
    m.from = settings_.callsign;
    m.text = typed;
    m.seq = record.header.seq;
    m.time = record.header.time;
    m.atMs = nowMs;
    m.state = State::Queued;
    queued_.push_back({m.id, std::move(bytes)});
    messages_.push_back(std::move(m));
    uint64_t id = messages_.back().id;
    trim();
    if (!paused_) writeQueued(nowMs);
    changed();
    return id;
}

uint64_t SessionChat::resend(uint64_t id, uint64_t nowMs)
{
    for (const Message& m : messages_)
    {
        if (m.id != id) continue;
        if (!canResend(m)) return 0;
        std::string text = m.text;   // send() may trim the list under m
        return send(text, nowMs);
    }
    return 0;
}

void SessionChat::setPaused(bool paused, uint64_t nowMs)
{
    paused_ = paused;
    if (!paused_ && phase_ == Phase::Connected) writeQueued(nowMs);
    changed();
}

//-------------------------------------------------------------------------
// The ports
//-------------------------------------------------------------------------

void SessionChat::write(const std::vector<uint8_t>& bytes, uint64_t nowMs)
{
    (void)nowMs;
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void SessionChat::writeQueued(uint64_t nowMs)
{
    if (!dataUp_) return;
    for (auto& q : queued_)
    {
        write(q.second, nowMs);
        for (Message& m : messages_)
        {
            if (m.id != q.first) continue;
            m.state = State::Sent;
            m.streamEnd = account_.written() + out_.size();
        }
    }
    if (!queued_.empty()) changed();
    queued_.clear();
}

std::vector<uint8_t> SessionChat::takeDataOutput(uint64_t nowMs)
{
    if (phase_ == Phase::Connected && dataUp_ && traffic_ != nullptr)
    {
        std::vector<uint8_t> more;
        traffic_->write(peer_, numbering_, account_.written() + out_.size(), nowMs, more);
        write(more, nowMs);
    }
    std::vector<uint8_t> out;
    out.swap(out_);
    if (!dataUp_) return {};
    account_.wrote(out.size(), nowMs);
    return out;
}

std::vector<protocol::Record> SessionChat::dataBytes(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    if (afterglowUntilMs_ != 0 && nowMs >= afterglowUntilMs_) endAfterglow();
    std::vector<protocol::Record> records;
    bool wasForeign = reader_.foreign();
    reader_.feed(bytes, length, records);
    if (reader_.foreign() != wasForeign) changed();

    if (afterglowUntilMs_ != 0)
    {
        // The last of the session that just ended: its farewell, its last words.
        for (const protocol::Record& r : records)
        {
            if (r.as<protocol::Text>() != nullptr || r.as<protocol::Status>() != nullptr) onRecord(r, nowMs);
        }
        if (farewellHeard_ && ending_ == Ending::LinkLost)
        {
            ending_ = Ending::LeftThere;
            changed();
        }
        return records;
    }

    bool up = phase_ == Phase::Connected || phase_ == Phase::Leaving;
    if (!up || connectedMs_ == 0)
    {
        // The far end's Hello can come before CONNECTED: it waits for it.
        early_.insert(early_.end(), records.begin(), records.end());
        return {};
    }
    if (length > 0) heard(nowMs);
    for (const protocol::Record& r : records) onRecord(r, nowMs);
    uint32_t offer = 0;
    size_t arrived = 0;
    if (traffic_ != nullptr && reader_.partialFileData(offer, arrived)) traffic_->arriving(peer_, offer, arrived);
    return records;
}

void SessionChat::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    if (const protocol::Hello* hello = record.as<protocol::Hello>())
    {
        if (!greetedTold_ && traffic_ != nullptr) traffic_->greeted(peer_, *hello);
        greetedTold_ = true;
        changed();
    }
    else if (const protocol::Text* text = record.as<protocol::Text>())
    {
        Message m;
        m.id = nextId_++;
        m.from = record.header.callsign;
        m.text = text->text;
        m.seq = record.header.seq;
        m.time = record.header.time;
        m.atMs = nowMs;
        messages_.push_back(m);
        trim();
        SessionChat::Event e;
        e.type = SessionChat::Event::Type::Heard;
        e.peer = peer_;
        e.id = m.id;
        emit(e);
        changed();
    }
    else if (const protocol::Status* status = record.as<protocol::Status>())
    {
        // The far end's farewell: it is leaving on purpose.
        if (status->state != protocol::Status::State::Qrl)
        {
            farewellHeard_ = true;
            changed();
        }
    }
    else if (record.as<protocol::Ack>() == nullptr && traffic_ != nullptr)
    {
        traffic_->onRecord(peer_, record, nowMs);
    }
}

void SessionChat::heard(uint64_t nowMs)
{
    lastHeardMs_ = std::max(lastHeardMs_, nowMs);
    waitingSinceMs_ = std::max(waitingSinceMs_, nowMs);
}

void SessionChat::settle()
{
    uint64_t acked = account_.acknowledged();
    for (Message& m : messages_)
    {
        if (m.ours && m.state == State::Sent && m.streamEnd <= acked)
        {
            m.state = State::Delivered;
            changed();
        }
    }
    if (traffic_ != nullptr && connectedMs_ != 0) traffic_->acknowledged(peer_, acked);
}

void SessionChat::checkFarewell(uint64_t nowMs)
{
    if (!farewellPending_) return;
    if (account_.read() < farewellEnd_ && nowMs < farewellMs_ + FAREWELL_WAIT_MS) return;
    farewellPending_ = false;
    link_.leave(nowMs);
}

void SessionChat::onEvent(const link::HostLink::Event& event, uint64_t nowMs)
{
    using Type = LinkEvent::Type;
    switch (event.type)
    {
        case Type::SessionCalling:
            if (phase_ != Phase::Calling)
            {
                // Called through the link directly: show it all the same.
                phase_ = Phase::Calling;
                peer_ = event.peer;
                incoming_ = false;
                tries_ = 0;
                sinceMs_ = nowMs;
                connectedMs_ = 0;
                messages_.clear();
                changed();
            }
            break;
        case Type::SessionConnected:
            begin(event.peer, event.incoming, event.number, nowMs);
            break;
        case Type::SessionEnded:
        {
            using End = link::HostLink::SessionEnd;
            Ending ending = Ending::LinkLost;
            switch (event.sessionEnd)
            {
                case End::NoAnswer:
                    ending = cancelling_ ? Ending::Cancelled : Ending::NoAnswer;
                    break;
                case End::Refused:
                    ending = Ending::Refused;
                    break;
                case End::Left:
                    ending = gaveUp_      ? Ending::NoAnswer
                             : cancelling_ ? Ending::Cancelled
                             : dataLost_   ? Ending::DataLost
                                           : Ending::Left;
                    break;
                case End::Ended:
                    ending = cancelling_     ? Ending::Cancelled
                             : gaveUp_        ? Ending::NoAnswer
                             : dataLost_      ? Ending::DataLost
                             : farewellHeard_ ? Ending::LeftThere
                             : phase_ == Phase::Leaving && connectedMs_ != 0 ? Ending::Left
                                                                             : Ending::LinkLost;
                    break;
                case End::HostLost:
                    ending = Ending::HostLost;
                    break;
            }
            if (phase_ == Phase::Calling && ending == Ending::LinkLost) ending = Ending::NoAnswer;
            finish(ending, nowMs);
            break;
        }
        case Type::Buffer:
        {
            if (!(phase_ == Phase::Connected || phase_ == Phase::Leaving) || connectedMs_ == 0) break;
            int before = account_.waiting();
            account_.buffer(event.count, nowMs);
            // A fall is the far end's acknowledgement: we hear it.
            if (event.count < before) heard(nowMs);
            if (before == 0 && event.count > 0) waitingSinceMs_ = std::max(waitingSinceMs_, nowMs);
            settle();
            checkFarewell(nowMs);
            changed();
            break;
        }
        case Type::Ptt:
            // Each call keys the transmitter.
            if (phase_ == Phase::Calling && event.on && !pttOn_)
            {
                tries_++;
                changed();
            }
            pttOn_ = event.on;
            break;
        case Type::ModeUsed:
            // While a broadcast of ours is with the host, the mode may be its.
            if ((phase_ == Phase::Connected || phase_ == Phase::Leaving) && !link_.burstWithHost()
                && event.text != mode_)
            {
                mode_ = event.text;
                changed();
            }
            break;
        default:
            break;
    }
}

void SessionChat::begin(const std::string& peer, bool incoming, int bandwidth, uint64_t nowMs)
{
    bool stopping = cancelling_ || gaveUp_;
    endAfterglow();
    if (incoming || phase_ != Phase::Calling) messages_.clear();
    phase_ = Phase::Connected;
    peer_ = peer;
    incoming_ = incoming;
    bandwidthHz_ = bandwidth;
    connectedMs_ = sinceMs_ = nowMs;
    lastHeardMs_ = waitingSinceMs_ = nowMs;
    mode_.clear();
    farewellHeard_ = farewellPending_ = false;
    greetedTold_ = false;
    if (!stopping) cancelling_ = gaveUp_ = false;
    dataLost_ = false;
    numbering_ = protocol::Numbering(settings_.callsign);
    account_.reset();
    out_.clear();
    queued_.clear();

    SessionChat::Event started;
    started.type = SessionChat::Event::Type::Started;
    started.peer = peer;
    started.incoming = incoming;
    emit(started);
    changed();

    if (stopping || !dataUp_)
    {
        // A call stopped as it was answered, or no data port to talk on.
        dataLost_ = !dataUp_ && !stopping;
        phase_ = Phase::Leaving;
        link_.leaveNow(nowMs);
        return;
    }

    protocol::Hello hello;
    hello.acceptsSessions = settings_.acceptsSessions;
    hello.locator = settings_.locator;
    hello.software = settings_.software;
    write(protocol::streamOpening(numbering_.record(hello, seconds(nowMs))), nowMs);

    // Late bytes of the last session made the reader give up: start clean.
    if (reader_.foreign() && early_.empty()) reader_.reset();
    std::vector<protocol::Record> early;
    early.swap(early_);
    for (const protocol::Record& r : early) onRecord(r, nowMs);
}

void SessionChat::finish(Ending ending, uint64_t nowMs)
{
    bool wasUp = connectedMs_ != 0 && (phase_ == Phase::Connected || phase_ == Phase::Leaving);
    for (Message& m : messages_)
    {
        if (m.ours && (m.state == State::Sent || m.state == State::Queued)) m.state = State::NotDelivered;
    }
    queued_.clear();
    if (wasUp && traffic_ != nullptr) traffic_->ended(peer_, nowMs);
    phase_ = Phase::Ended;
    ending_ = ending;
    sinceMs_ = nowMs;
    pttOn_ = false;
    farewellPending_ = false;
    // Ready for the next session's stream (docs/PROTOCOL.md, "Sessions"),
    // after a moment for a farewell still on its way.
    early_.clear();
    out_.clear();
    if (ending == Ending::LinkLost)
        afterglowUntilMs_ = std::max<uint64_t>(nowMs + AFTERGLOW_MS, 1);
    else
        reader_.reset();

    SessionChat::Event e;
    e.type = SessionChat::Event::Type::Ended;
    e.peer = peer_;
    e.ending = ending;
    emit(e);
    changed();
}

void SessionChat::endAfterglow()
{
    if (afterglowUntilMs_ == 0) return;
    afterglowUntilMs_ = 0;
    reader_.reset();
}

void SessionChat::dataPort(bool up, uint64_t nowMs)
{
    if (up == dataUp_) return;
    dataUp_ = up;
    if (!up)
    {
        out_.clear();
        if (active())
        {
            dataLost_ = true;
            farewellPending_ = false;
            if (phase_ == Phase::Calling) cancelling_ = false;
            phase_ = Phase::Leaving;
            link_.leaveNow(nowMs);
        }
        else
        {
            afterglowUntilMs_ = 0;
            reader_.reset();
            early_.clear();
        }
    }
    changed();
}

void SessionChat::tick(uint64_t nowMs)
{
    if (afterglowUntilMs_ != 0 && nowMs >= afterglowUntilMs_) endAfterglow();
    if (phase_ == Phase::Calling && nowMs >= sinceMs_ + CALL_GIVE_UP_MS)
    {
        gaveUp_ = true;
        phase_ = Phase::Leaving;
        link_.leaveNow(nowMs);
        changed();
    }
    checkFarewell(nowMs);
}

std::vector<SessionChat::Event> SessionChat::takeEvents()
{
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

//-------------------------------------------------------------------------
// What the tab shows
//-------------------------------------------------------------------------

int SessionChat::callTry() const
{
    return std::max(1, std::min(CALL_TRIES, tries_));
}

bool SessionChat::quiet(uint64_t nowMs) const
{
    bool up = phase_ == Phase::Connected || phase_ == Phase::Leaving;
    return up && connectedMs_ != 0 && account_.waiting() > 0 && nowMs >= waitingSinceMs_ + QUIET_MS;
}

uint64_t SessionChat::quietForMs(uint64_t nowMs) const
{
    return nowMs > waitingSinceMs_ ? nowMs - waitingSinceMs_ : 0;
}

bool SessionChat::trafficBusy() const
{
    return traffic_ != nullptr && active() && traffic_->busy(peer_);
}

const Message* SessionChat::find(uint64_t id) const
{
    for (const Message& m : messages_)
    {
        if (m.id == id) return &m;
    }
    return nullptr;
}

void SessionChat::trim()
{
    while (messages_.size() > MAX_MESSAGES) messages_.pop_front();
}

SessionChat::Strip SessionChat::strip(uint64_t nowMs, long utcOffsetS) const
{
    Strip s;
    s.live = active();
    std::string at = clockTime(sinceMs_, utcOffsetS);
    int waiting = account_.waiting();
    std::string bytes = std::to_string(waiting) + (waiting == 1 ? " byte" : " bytes");
    switch (phase_)
    {
        case Phase::None:
            s.text = "No session. Call a station from the Station menu or its right-click menu, "
                     "or wait for one to call you.";
            break;
        case Phase::Calling:
            s.text = "Calling " + peer_ + ", try " + std::to_string(callTry()) + " of " + std::to_string(CALL_TRIES);
            break;
        case Phase::Connected:
        {
            if (quiet(nowMs))
            {
                s.text = "Link quiet" + std::string(DOT) + clockLength(quietForMs(nowMs)) + DOT
                         + "data2g-host is still retrying" + DOT + bytes + " waiting";
                s.amber = true;
                break;
            }
            bool fresh = incoming_ && nowMs < connectedMs_ + 60000;
            s.text = fresh ? peer_ + " started a session with you" : "Connected to " + peer_;
            s.text += DOT + sessionLength(nowMs - std::min(nowMs, connectedMs_));
            if (bandwidthHz_ > 0) s.text += DOT + std::to_string(bandwidthHz_) + " Hz";
            if (!mode_.empty()) s.text += DOT + mode_;
            if (waiting > 0) s.text += DOT + bytes + " waiting";
            s.text += DOT + std::string("heard ") + broadcast::heardAgo(lastHeardMs_, nowMs);
            if (reader_.foreign()) s.text += DOT + peer_ + "'s end isn't speaking Ragtime: nothing more is read from it";
            break;
        }
        case Phase::Leaving:
            if (cancelling_ || gaveUp_ || connectedMs_ == 0)
                s.text = "Stopping the call to " + peer_;
            else if (waiting > 0)
                s.text = "Leaving, waiting for " + bytes + " to be acknowledged";
            else
                s.text = "Leaving the session with " + peer_;
            s.amber = quiet(nowMs);
            break;
        case Phase::Ended:
            switch (ending_)
            {
                case Ending::NoAnswer:
                    s.text = peer_ + " didn't answer (" + at + ")";
                    break;
                case Ending::Refused:
                    s.text = "data2g-host wouldn't call " + peer_ + " (" + at + ")";
                    break;
                case Ending::Cancelled:
                    s.text = "Call to " + peer_ + " cancelled at " + at;
                    break;
                case Ending::Left:
                    s.text = "You left the session with " + peer_ + " at " + at + ", after "
                             + sessionLength(sinceMs_ - std::min(sinceMs_, connectedMs_));
                    break;
                case Ending::LeftThere:
                    s.text = "Session ended by " + peer_ + " at " + at;
                    break;
                case Ending::LinkLost:
                {
                    uint64_t silent = sinceMs_ > lastHeardMs_ ? sinceMs_ - lastHeardMs_ : 0;
                    if (silent >= 60000)
                        s.text = "Link lost at " + at + " after " + sessionLength(silent) + " without contact";
                    else
                        s.text = "The session with " + peer_ + " ended at " + at;
                    break;
                }
                case Ending::HostLost:
                    s.text = "data2g-host went away at " + at + ", and the session with " + peer_ + " with it";
                    break;
                case Ending::DataLost:
                    s.text = "The session data port closed at " + at + ", so the session with " + peer_ + " ended";
                    break;
            }
            break;
    }
    return s;
}

} // namespace session
} // namespace ragtime
