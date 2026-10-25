#include "ragtime/protocol/Stream.h"

#include <utility>

namespace ragtime
{
namespace protocol
{

namespace
{

constexpr uint32_t MAX_ITEM = 0xFFFFFF;

} // namespace

Header Numbering::header(RecordType type, uint32_t time, uint32_t item)
{
    Header header;
    header.callsign = callsign_;
    header.time = time;
    if (isItem(type))
    {
        newest_ = newest_ >= MAX_ITEM ? 1 : newest_ + 1;
        header.seq = newest_;
    }
    else
    {
        header.seq = isPartOfItem(type) ? item : newest_;
    }
    return header;
}

Record Numbering::record(Body body, uint32_t time, uint32_t item)
{
    Record record;
    record.body = std::move(body);
    record.header = header((RecordType)record.type(), time, item);
    return record;
}

bool StreamReader::notOurs()
{
    foreign_ = true;
    pending_.clear();
    return false;
}

bool StreamReader::feed(const uint8_t* bytes, size_t length, std::vector<Record>& out)
{
    if (foreign_) return false;
    pending_.insert(pending_.end(), bytes, bytes + length);

    size_t used = 0;
    if (!opened_)
    {
        if (pending_.empty()) return true;
        if (pending_[0] != FRAME_BYTE) return notOurs();
        opened_ = true;
        used = 1;
    }

    const uint8_t* begin = pending_.data();
    const uint8_t* at = begin + used;
    const uint8_t* end = begin + pending_.size();
    while (at < end)
    {
        // The first record must be a Hello.
        if (!greeted_ && at[0] != (uint8_t)RecordType::Hello) return notOurs();
        Record record;
        DecodeResult result = decodeRecord(at, end, record);
        if (result == DecodeResult::Short) break;
        if (result == DecodeResult::Invalid) return notOurs();
        if (result == DecodeResult::Malformed)
        {
            if (!greeted_) return notOurs();
            malformed_++;
            continue;
        }
        if (!greeted_)
        {
            greeted_ = true;
            hello_ = *record.as<Hello>();
            peer_ = record.header.callsign;
        }
        out.push_back(std::move(record));
    }
    pending_.erase(pending_.begin(), pending_.begin() + (at - begin));
    return true;
}

void StreamReader::reset()
{
    pending_.clear();
    opened_ = false;
    greeted_ = false;
    foreign_ = false;
    hello_ = Hello();
    peer_.clear();
    malformed_ = 0;
}

bool StreamReader::partialFileData(uint32_t& offer, size_t& bytes) const
{
    uint32_t offset = 0;
    const uint8_t* data = nullptr;
    return partialFileData(offer, offset, data, bytes);
}

bool StreamReader::partialFileData(uint32_t& offer, uint32_t& offset, const uint8_t*& data, size_t& bytes) const
{
    // What feed() leaves is the start of one record, never more.
    if (foreign_ || !greeted_ || pending_.empty() || pending_[0] != (uint8_t)RecordType::FileData) return false;
    const uint8_t* end = pending_.data() + pending_.size();
    size_t length = 0, fieldBytes = 0;
    if (readLength(pending_.data() + 1, end, length, fieldBytes) != LengthRead::Ok) return false;
    Reader in(pending_.data() + 1 + fieldBytes, end);
    std::string call;
    uint32_t seq = 0, time = 0, at = 0;
    if (!in.callsign(call) || !in.u24(seq) || !in.u32(time) || !in.u32(at)) return false;
    offer = seq;
    offset = at;
    bytes = in.left();
    data = end - bytes;
    return true;
}

std::vector<uint8_t> streamOpening(const Record& hello)
{
    std::vector<uint8_t> out(1, FRAME_BYTE);
    if (hello.as<Hello>() == nullptr || !encodeRecord(hello, out)) return {};
    return out;
}

} // namespace protocol
} // namespace ragtime
