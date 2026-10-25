#include "ragtime/session/StreamLog.h"

#include <algorithm>
#include <iterator>

#include "ragtime/protocol/Fields.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace session
{

void StreamLog::wrote(const uint8_t* bytes, size_t length, uint64_t message)
{
    if (length == 0) return;
    bytes_.insert(bytes_.end(), bytes, bytes + length);
    parse(message);
}

// Marks out the records that have begun since the last one.
void StreamLog::parse(uint64_t message)
{
    uint64_t at = spans_.empty() ? base_ : spans_.back().end;
    while (at < end())
    {
        size_t i = (size_t)(at - base_);
        Span span;
        span.start = at;
        span.message = message;
        message = 0;
        if (at == 0 && bytes_[i] == protocol::FRAME_BYTE)
        {
            span.end = 1;
        }
        else
        {
            uint8_t field[2] = {0, 0};
            size_t have = std::min<size_t>(2, bytes_.size() - i - 1);
            for (size_t k = 0; k < have; k++) field[k] = bytes_[i + 1 + k];
            size_t length = 0, fieldBytes = 0;
            protocol::LengthRead read = protocol::readLength(field, field + have, length, fieldBytes);
            // Never short: we write whole records. One that isn't ours to
            // read runs to the end of what was written.
            if (read != protocol::LengthRead::Ok)
                span.end = end();
            else
                span.end = at + 1 + fieldBytes + length;
            span.type = bytes_[i];
        }
        spans_.push_back(span);
        at = span.end;
    }
}

void StreamLog::acknowledged(uint64_t acked)
{
    while (!spans_.empty() && spans_.front().end <= acked && spans_.front().end <= end())
    {
        uint64_t drop = spans_.front().end - base_;
        bytes_.erase(bytes_.begin(), bytes_.begin() + (long)drop);
        base_ = spans_.front().end;
        spans_.pop_front();
    }
}

void StreamLog::reset()
{
    *this = StreamLog();
}

bool StreamLog::cut(uint64_t c, Cut& out)
{
    out = Cut();
    if (c < base_ || c > end()) return false;
    auto first = std::find_if(spans_.begin(), spans_.end(), [&](const Span& s) { return s.end > c; });
    if (first != spans_.end() && first->start < c)
    {
        // The record the cut fell in: its tail goes again.
        uint64_t to = std::min(first->end, end());
        out.tail.assign(bytes_.begin() + (long)(c - base_), bytes_.begin() + (long)(to - base_));
        ++first;
    }
    for (auto it = first; it != spans_.end(); ++it)
    {
        out.lost.push_back(*it);
        uint64_t to = std::min(it->end, end());
        out.lostBytes.emplace_back(bytes_.begin() + (long)(it->start - base_), bytes_.begin() + (long)(to - base_));
    }
    spans_.erase(first, spans_.end());
    bytes_.erase(bytes_.begin() + (long)(c - base_), bytes_.end());
    return true;
}

} // namespace session
} // namespace ragtime
