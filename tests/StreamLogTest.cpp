// The log of our session stream that mends a cut: records marked out as
// they are written, let go as they are acknowledged, and a cut's tail and
// lost records.
#include <cstdint>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/protocol/Records.h"
#include "ragtime/protocol/Stream.h"
#include "ragtime/session/StreamLog.h"

using namespace ragtime;
using session::StreamLog;

namespace
{

std::vector<uint8_t> text(protocol::Numbering& numbering, const std::string& words)
{
    protocol::Text t;
    t.text = words;
    return protocol::encodeRecord(numbering.record(t, 1792627200));
}

std::vector<uint8_t> fileData(protocol::Numbering& numbering, size_t bytes)
{
    protocol::FileData d;
    d.bytes.assign(bytes, 0x5A);
    return protocol::encodeRecord(numbering.record(std::move(d), 1792627200, 1));
}

std::vector<uint8_t> join(const std::vector<std::vector<uint8_t>>& parts)
{
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void testSpans()
{
    protocol::Numbering numbering("AG7EW");
    protocol::Hello hello;
    std::vector<uint8_t> opening = protocol::streamOpening(numbering.record(hello, 1792627200));
    std::vector<uint8_t> hi = text(numbering, "hello");
    std::vector<uint8_t> big = fileData(numbering, 300); // a length field of two bytes
    StreamLog log;
    log.wrote(opening);
    log.wrote(hi, 7);
    log.wrote(big);
    CHECK_EQ(log.spans().size(), (size_t)4);
    if (log.spans().size() == 4)
    {
        CHECK(log.spans()[0].start == 0 && log.spans()[0].end == 1 && log.spans()[0].type == 0);
        CHECK(log.spans()[1].type == (uint8_t)protocol::RecordType::Hello && log.spans()[1].end == opening.size());
        CHECK(log.spans()[2].message == 7 && log.spans()[2].end == opening.size() + hi.size());
        CHECK(log.spans()[3].type == (uint8_t)protocol::RecordType::FileData
              && log.spans()[3].end == opening.size() + hi.size() + big.size() && log.spans()[3].message == 0);
    }
    CHECK_EQ(log.end(), (uint64_t)(opening.size() + hi.size() + big.size()));

    // Two records in one write.
    StreamLog two;
    two.wrote(join({hi, big}));
    CHECK(two.spans().size() == 2 && two.spans()[1].start == hi.size());

    // Acknowledged: whole records go, one still partly unacknowledged stays.
    log.acknowledged(opening.size() + 3);
    CHECK_EQ(log.start(), (uint64_t)opening.size());
    CHECK_EQ(log.spans().size(), (size_t)2);
}

void testACutInsideARecord()
{
    protocol::Numbering numbering("AG7EW");
    std::vector<uint8_t> a = fileData(numbering, 200);
    std::vector<uint8_t> hi = text(numbering, "hi");
    std::vector<uint8_t> b = fileData(numbering, 200);
    std::vector<uint8_t> all = join({a, hi, b});
    StreamLog log;
    log.wrote(a);
    log.wrote(hi, 9);
    log.wrote(b);

    // The cut falls 50 bytes into the first record.
    StreamLog::Cut cut;
    CHECK(log.cut(50, cut));
    CHECK(cut.tail == std::vector<uint8_t>(a.begin() + 50, a.end()));
    CHECK_EQ(cut.lost.size(), (size_t)2);
    CHECK_EQ(cut.lostBytes.size(), (size_t)2);
    if (cut.lost.size() == 2)
    {
        CHECK(cut.lost[0].message == 9 && cut.lostBytes[0] == hi);
        CHECK(cut.lost[1].type == (uint8_t)protocol::RecordType::FileData && cut.lostBytes[1] == b);
    }
    CHECK_EQ(log.end(), (uint64_t)50);

    // The tail is written back, then a new record: the log reads on.
    log.wrote(cut.tail);
    log.wrote(hi, 10);
    CHECK_EQ(log.spans().size(), (size_t)2);
    CHECK(log.spans().size() == 2 && log.spans()[1].start == a.size() && log.spans()[1].message == 10);

    // Between records, there is no tail.
    StreamLog clean;
    clean.wrote(all);
    CHECK(clean.cut(a.size(), cut));
    CHECK(cut.tail.empty() && cut.lost.size() == 2);
    // At the end, nothing is lost.
    CHECK(clean.cut(clean.end(), cut));
    CHECK(cut.tail.empty() && cut.lost.empty());
    // Before what the log still holds, or past its end: no.
    StreamLog acked;
    acked.wrote(all);
    acked.acknowledged(a.size());
    CHECK(!acked.cut(10, cut));
    CHECK(!acked.cut(acked.end() + 1, cut));
}

} // namespace

int main()
{
    testSpans();
    testACutInsideARecord();
    return ragtime_test::finish();
}
