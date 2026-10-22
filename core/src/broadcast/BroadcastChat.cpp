#include "ragtime/broadcast/BroadcastChat.h"

#include <algorithm>
#include <utility>

namespace ragtime
{
namespace broadcast
{

namespace
{

using State = Message::State;
using Event = link::HostLink::Event;
using BurstState = link::HostLink::BurstState;

uint32_t seconds(uint64_t nowMs)
{
    return (uint32_t)(nowMs / 1000);
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), isSpace);
}

// The start of the UTF-8 character at or before at.
size_t characterStart(const std::string& text, size_t at)
{
    while (at > 0 && at < text.size() && ((uint8_t)text[at] & 0xC0) == 0x80) at--;
    return at;
}

State fromBurst(BurstState state)
{
    switch (state)
    {
        case BurstState::Queued:
            return State::Queued;
        case BurstState::Sending:
            return State::Sending;
        case BurstState::Held:
            return State::Held;
        case BurstState::Sent:
            return State::Sent;
        case BurstState::NotSent:
            return State::NotSent;
    }
    return State::NotSent;
}

} // namespace

const char* stateLabel(Message::State state)
{
    switch (state)
    {
        case State::Heard:
            return "";
        case State::Queued:
            return "Queued";
        case State::Sending:
            return "Sending";
        case State::Held:
            return "Waiting for the session to go idle";
        case State::Sent:
            return "Sent";
        case State::NotSent:
            return "Not sent";
        case State::Cancelled:
            return "Cancelled";
        case State::Acknowledged:
            return "Acknowledged";
    }
    return "";
}

BroadcastChat::BroadcastChat(link::HostLink& link) : link_(link)
{
}

void BroadcastChat::setSettings(const Settings& settings, uint32_t newestItem)
{
    Settings next = settings;
    next.callsign = protocol::normalCallsign(settings.callsign);
    bool newCall = next.callsign != settings_.callsign || numbering_.callsign() != next.callsign;
    bool changed = newCall || next.locator != settings_.locator || next.acceptsSessions != settings_.acceptsSessions
                   || next.software != settings_.software;
    settings_ = next;
    if (newCall) numbering_ = protocol::Numbering(settings_.callsign, newestItem);
    if (changed) helloSent_ = false;
    version_++;
}

bool BroadcastChat::addressee(const std::string& typed, std::string& call, std::string& rest)
{
    size_t at = 0;
    while (at < typed.size() && isSpace(typed[at])) at++;
    if (at >= typed.size() || typed[at] != '@') return false;
    size_t end = at + 1;
    while (end < typed.size() && !isSpace(typed[end]) && typed[end] != ',' && typed[end] != ':') end++;
    std::string normal = protocol::normalCallsign(typed.substr(at + 1, end - at - 1));
    if (normal.empty()) return false;
    while (end < typed.size() && (isSpace(typed[end]) || typed[end] == ',' || typed[end] == ':')) end++;
    if (end >= typed.size()) return false;
    call = normal;
    rest = typed.substr(end);
    return true;
}

std::vector<text::HamText::PhraseSpan> BroadcastChat::phrases(const std::string& typed)
{
    std::string call, rest;
    if (!addressee(typed, call, rest)) return text::HamText::phraseSpans(typed);
    std::vector<text::HamText::PhraseSpan> spans = text::HamText::phraseSpans(rest);
    for (text::HamText::PhraseSpan& span : spans) span.start += typed.size() - rest.size();
    return spans;
}

protocol::Record BroadcastChat::helloRecord(uint32_t time)
{
    protocol::Hello hello;
    hello.acceptsSessions = settings_.acceptsSessions;
    hello.locator = settings_.locator;
    hello.software = settings_.software;
    return numbering_.record(hello, time);
}

bool BroadcastChat::helloDue(uint64_t nowMs) const
{
    return !helloSent_ || nowMs >= helloMs_ + HELLO_EVERY_MS;
}

std::vector<std::string> BroadcastChat::split(const std::string& text, const std::string& to, size_t firstRoom)
{
    // A Text of these bytes, with a header like ours: its seq and time are
    // fixed-size, so any will do for measuring.
    auto bytes = [&](const std::string& piece) {
        protocol::Record r;
        r.header.callsign = settings_.callsign;
        protocol::Text t;
        t.text = piece;
        t.to = to;
        t.ackWanted = !to.empty();
        r.body = t;
        return encodeRecord(r).size();
    };

    std::vector<std::string> out;
    size_t at = 0;
    size_t room = firstRoom;
    while (at < text.size())
    {
        size_t budget = protocol::MAX_BROADCAST_FRAME - 1 - room;
        std::string rest = text.substr(at);
        if (bytes(rest) <= budget)
        {
            out.push_back(rest);
            break;
        }
        // The longest start that fits: coded text grows with its length,
        // near enough, so halve the gap until it closes.
        size_t lo = 0, hi = rest.size();
        while (lo + 1 < hi)
        {
            size_t mid = characterStart(rest, (lo + hi) / 2);
            if (mid <= lo) mid = lo + 1;
            if (bytes(rest.substr(0, mid)) <= budget) lo = mid;
            else hi = mid;
        }
        size_t cut = characterStart(rest, lo);
        if (cut == 0) cut = std::min(rest.size(), (size_t)1);
        // Between words, if there is a space in the second half.
        size_t space = rest.rfind(' ', cut);
        if (space != std::string::npos && space > cut / 2 && space < cut)
        {
            out.push_back(rest.substr(0, space));
            at += space + 1;
        }
        else
        {
            out.push_back(rest.substr(0, cut));
            at += cut;
        }
        room = 0;
    }
    return out;
}

std::vector<uint64_t> BroadcastChat::send(const std::string& typed, const std::string& mode, uint64_t nowMs)
{
    std::vector<uint64_t> ids;
    if (settings_.callsign.empty() || blank(typed) || typed.size() > MAX_TYPED) return ids;

    std::string to, text = typed;
    std::string call, rest;
    if (addressee(typed, call, rest))
    {
        to = call;
        text = rest;
    }

    uint32_t time = seconds(nowMs);
    std::vector<protocol::Record> first;
    if (helloDue(nowMs)) first.push_back(helloRecord(time));
    size_t room = first.empty() ? 0 : protocol::encodeRecord(first.front()).size();

    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint64_t> made;
    std::vector<protocol::Record> items;
    for (const std::string& piece : split(text, to, room))
    {
        protocol::Text t;
        t.text = piece;
        t.to = to;
        t.ackWanted = !to.empty();
        protocol::Record record = numbering_.record(t, time);
        std::vector<protocol::Record> records = frames.empty() ? first : std::vector<protocol::Record>();
        records.push_back(record);
        std::vector<uint8_t> frame = protocol::encodeFrame(records);
        if (frame.empty()) continue;
        frames.push_back(std::move(frame));

        Message m;
        m.id = nextId_++;
        m.ours = true;
        m.from = settings_.callsign;
        m.to = to;
        m.ackWanted = t.ackWanted;
        m.text = piece;
        m.seq = record.header.seq;
        m.time = time;
        m.atMs = nowMs;
        m.state = State::Queued;
        m.mode = mode;
        records_[m.id] = record;
        items.push_back(record);
        messages_.push_back(m);
        made.push_back(m.id);
    }
    if (frames.empty()) return ids;

    if (!first.empty())
    {
        helloSent_ = true;
        helloMs_ = nowMs;
    }
    uint64_t burst = link_.broadcast(frames, mode, nowMs);
    for (Message& m : messages_)
    {
        if (std::find(made.begin(), made.end(), m.id) != made.end()) m.burst = burst;
    }
    if (onItems_ && burst != 0) onItems_(items, burst, mode, nowMs);
    trim();
    version_++;
    return made;
}

bool BroadcastChat::checkIn(const std::string& mode, uint64_t nowMs)
{
    if (settings_.callsign.empty()) return false;
    std::vector<uint8_t> frame = protocol::encodeFrame({helloRecord(seconds(nowMs))});
    if (frame.empty()) return false;
    helloSent_ = true;
    helloMs_ = nowMs;
    return link_.broadcast({frame}, mode, nowMs) != 0;
}

bool BroadcastChat::announce(protocol::Status::State state, const std::string& partner, const std::string& mode,
                             uint64_t nowMs)
{
    using StatusState = protocol::Status::State;
    if (settings_.callsign.empty()) return false;
    if (state == StatusState::Qrv)
    {
        if (!qrlOut_) return false;
        qrlOut_ = false;
        // Nobody heard the QRL: nothing to take back on the air.
        if (link_.cancel(qrlBurst_)) return false;
    }
    protocol::Status status;
    status.state = state;
    if (state == StatusState::Qrl) status.partner = protocol::normalCallsign(partner);
    std::vector<uint8_t> frame = protocol::encodeFrame({numbering_.record(status, seconds(nowMs))});
    if (frame.empty()) return false;
    uint64_t id = link_.broadcast({frame}, mode, nowMs);
    if (state == StatusState::Qrl)
    {
        qrlBurst_ = id;
        qrlOut_ = id != 0;
    }
    return id != 0;
}

Message* BroadcastChat::findMine(uint64_t id)
{
    for (Message& m : messages_)
    {
        if (m.id == id) return m.ours ? &m : nullptr;
    }
    return nullptr;
}

const Message* BroadcastChat::find(uint64_t id) const
{
    for (const Message& m : messages_)
    {
        if (m.id == id) return &m;
    }
    return nullptr;
}

bool BroadcastChat::cancel(uint64_t id)
{
    Message* m = findMine(id);
    if (m == nullptr || m->state != State::Queued) return false;
    uint64_t burst = m->burst;
    if (!link_.cancel(burst)) return false;
    for (Message& other : messages_)
    {
        if (other.ours && other.burst == burst) other.state = State::Cancelled;
    }
    version_++;
    return true;
}

bool BroadcastChat::canResend(const Message& message)
{
    return message.ours && (message.state == State::Sent || message.state == State::NotSent
                            || message.state == State::Cancelled || message.state == State::Acknowledged);
}

bool BroadcastChat::resend(uint64_t id, const std::string& mode, uint64_t nowMs)
{
    Message* m = findMine(id);
    auto record = records_.find(id);
    if (m == nullptr || record == records_.end() || !canResend(*m)) return false;
    std::vector<uint8_t> frame = protocol::encodeFrame({record->second});
    if (frame.empty()) return false;
    m->state = State::Queued;
    m->mode = mode;
    m->burst = link_.broadcast({frame}, mode, nowMs);
    if (onItems_ && m->burst != 0) onItems_({record->second}, m->burst, mode, nowMs);
    version_++;
    return true;
}

protocol::Record BroadcastChat::record(protocol::Body body, uint32_t time, uint32_t item)
{
    return numbering_.record(std::move(body), time, item);
}

uint64_t BroadcastChat::addFile(uint64_t file, bool ours, const std::string& from, const std::string& name,
                                uint64_t nowMs)
{
    Message m;
    m.id = nextId_++;
    m.ours = ours;
    m.from = from;
    m.text = name;
    m.time = seconds(nowMs);
    m.atMs = nowMs;
    m.state = State::Heard;
    m.file = file;
    messages_.push_back(std::move(m));
    trim();
    version_++;
    return messages_.back().id;
}

std::vector<protocol::Record> BroadcastChat::onEvent(const Event& event, uint64_t nowMs)
{
    std::vector<protocol::Record> heard;
    switch (event.type)
    {
        case Event::Type::GroupOpen:
            // A fresh start with the host: the next message says hello.
            helloSent_ = false;
            break;
        case Event::Type::Burst:
        {
            if (ackBursts_.count(event.id) != 0)
            {
                if (event.burstState == BurstState::Sent || event.burstState == BurstState::NotSent)
                    ackBursts_.erase(event.id);
                break;
            }
            for (Message& m : messages_)
            {
                if (!m.ours || m.burst != event.id) continue;
                if (m.state == State::Cancelled || m.state == State::Acknowledged) continue;
                m.state = fromBurst(event.burstState);
                version_++;
            }
            break;
        }
        case Event::Type::ModeUsed:
            for (Message& m : messages_)
            {
                if (m.ours && m.state == State::Sending)
                {
                    m.mode = event.text;
                    version_++;
                }
            }
            break;
        case Event::Type::Heard:
        {
            if (!protocol::decodeFrame(event.frame, heard) && heard.empty()) foreign_++;
            for (const protocol::Record& r : heard) onRecord(r, nowMs);
            flushAcks(std::string(), nowMs);
            break;
        }
        default:
            break;
    }
    return heard;
}

void BroadcastChat::onRecord(const protocol::Record& record, uint64_t nowMs)
{
    const std::string& from = record.header.callsign;
    if (from.empty() || from == settings_.callsign) return;

    if (const protocol::Text* text = record.as<protocol::Text>())
    {
        bool toMe = !settings_.callsign.empty() && text->to == settings_.callsign;
        // Asked again: our Ack went missing, so it goes again.
        if (toMe && text->ackWanted)
        {
            protocol::Ack ack;
            ack.to = from;
            ack.item = record.header.seq;
            acksDue_.push_back(numbering_.record(ack, seconds(nowMs)));
        }
        ItemKey key{from, record.header.seq, record.header.time};
        if (!seen_.insert(key).second) return;
        Message m;
        m.id = nextId_++;
        m.from = from;
        m.to = text->to;
        m.ackWanted = text->ackWanted;
        m.text = text->text;
        m.seq = record.header.seq;
        m.time = record.header.time;
        m.atMs = nowMs;
        m.state = State::Heard;
        messages_.push_back(std::move(m));
        trim();
        version_++;
    }
    else if (const protocol::Ack* ack = record.as<protocol::Ack>())
    {
        if (ack->to != settings_.callsign) return;
        for (Message& m : messages_)
        {
            if (m.ours && m.to == from && m.seq == ack->item && m.state != State::Acknowledged)
            {
                m.state = State::Acknowledged;
                version_++;
            }
        }
    }
}

void BroadcastChat::flushAcks(const std::string& mode, uint64_t nowMs)
{
    if (acksDue_.empty()) return;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<protocol::Record> frame;
    size_t bytes = 1;
    for (const protocol::Record& ack : acksDue_)
    {
        size_t size = protocol::encodeRecord(ack).size();
        if (!frame.empty() && bytes + size > protocol::MAX_BROADCAST_FRAME)
        {
            frames.push_back(protocol::encodeFrame(frame));
            frame.clear();
            bytes = 1;
        }
        frame.push_back(ack);
        bytes += size;
    }
    frames.push_back(protocol::encodeFrame(frame));
    acksDue_.clear();
    frames.erase(std::remove_if(frames.begin(), frames.end(), [](const std::vector<uint8_t>& f) { return f.empty(); }),
                 frames.end());
    uint64_t id = link_.broadcast(frames, mode, nowMs);
    if (id != 0) ackBursts_.insert(id);
}

void BroadcastChat::trim()
{
    while (messages_.size() > MAX_MESSAGES)
    {
        records_.erase(messages_.front().id);
        messages_.pop_front();
    }
}

} // namespace broadcast
} // namespace ragtime
