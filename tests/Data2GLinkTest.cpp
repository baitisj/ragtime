// KISS ports and ACKMODE, the command port's lines and the modes list.
// The KISS and command-line checks are ported from Glissando's Data2GTest.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Check.h"
#include "ragtime/link/Data2GLink.h"

using namespace ragtime::link;

namespace
{

void testKissPortsAndAckMode()
{
    std::vector<uint8_t> data = {0x01, 0xC0, 0xDB, 0x7F};
    std::vector<uint8_t> stream = kissEncode(3, data);
    std::vector<uint8_t> ack = kissEncodeAckMode(12, 0xC0DB, data); // 12: the command byte is FEND
    stream.insert(stream.end(), ack.begin(), ack.end());
    // A TXDELAY command, which is not passed on.
    std::vector<uint8_t> txdelay = {0xC0, 0x01, 0x32, 0xC0};
    stream.insert(stream.end(), txdelay.begin(), txdelay.end());

    KissDecoder decoder;
    std::vector<KissFrame> frames;
    for (uint8_t b : stream) decoder.feed(&b, 1, frames);
    CHECK_EQ(frames.size(), (size_t)2);
    if (frames.size() != 2) return;
    CHECK(frames[0].port == 3 && frames[0].command == KISS_DATA && frames[0].payload == data);
    CHECK(frames[1].port == 12 && frames[1].command == KISS_ACKMODE);
    std::vector<uint8_t> tagged = {0xC0, 0xDB};
    tagged.insert(tagged.end(), data.begin(), data.end());
    CHECK(frames[1].payload == tagged);

    // All at once, and after a reset mid-frame.
    frames.clear();
    decoder.reset();
    std::vector<uint8_t> broken = {0xC0, 0x00, 0x11};
    decoder.feed(broken.data(), broken.size(), frames);
    decoder.reset();
    decoder.feed(stream.data(), stream.size(), frames);
    CHECK_EQ(frames.size(), (size_t)2);
}

void testCommandLines()
{
    LineSplitter splitter;
    std::vector<std::string> lines;
    const char* part1 = "PTT ON\rBUSY O";
    const char* part2 = "FF\rMODE n10-qpsk-r1/5\rMODE qpsk-r1/3 1200 76 64 1.78 74.36\rBCAST PORT 2\r"
                        "BCAST 2 HEARD VK3ABC\rBCAST 2 DROPPED 3\rCONNECTED W1AW VK3ABC 2300\rBUFFER 120\rOK\r";
    splitter.feed(part1, std::strlen(part1), lines);
    splitter.feed(part2, std::strlen(part2), lines);
    CHECK_EQ(lines.size(), (size_t)10);
    if (lines.size() != 10) return;

    using Type = CommandEvent::Type;
    CommandEvent e = parseCommandLine(lines[0]);
    CHECK(e.type == Type::Ptt && e.on);
    e = parseCommandLine(lines[1]);
    CHECK(e.type == Type::Busy && !e.on);
    e = parseCommandLine(lines[2]);
    CHECK(e.type == Type::Mode && e.text == "n10-qpsk-r1/5");
    e = parseCommandLine(lines[3]);
    CHECK(e.type == Type::ModeLine && e.modeInfo.name == "qpsk-r1/3" && e.modeInfo.bandwidthHz == 1200 &&
          e.modeInfo.bytesPerCodeword == 76 && e.modeInfo.maxCodewords == 64 &&
          std::fabs(e.modeInfo.secondsAtMax - 74.36) < 1e-6);
    e = parseCommandLine(lines[4]);
    CHECK(e.type == Type::BcastPort && e.number == 2);
    e = parseCommandLine(lines[5]);
    CHECK(e.type == Type::BcastHeard && e.number == 2 && e.text == "VK3ABC");
    e = parseCommandLine(lines[6]);
    CHECK(e.type == Type::BcastDropped && e.count == 3);
    e = parseCommandLine(lines[7]);
    CHECK(e.type == Type::Connected && e.text == "W1AW" && e.peer == "VK3ABC" && e.number == 2300);
    e = parseCommandLine(lines[8]);
    CHECK(e.type == Type::Buffer && e.count == 120);
    CHECK(parseCommandLine(lines[9]).type == Type::Ok);
    CHECK(parseCommandLine("IAMALIVE").type == Type::Other);
    CHECK(parseCommandLine("BCAST 2 LOST 4").type == Type::BcastLost);
    CHECK(parseCommandLine("WRONG").type == Type::Wrong);
    CHECK(parseCommandLine("DISCONNECTED").type == Type::Disconnected);
    CHECK(parseCommandLine("connected k1abc w1aw").text == "K1ABC");
    // A MODE line that doesn't parse is not a mode.
    CHECK(parseCommandLine("MODE qpsk-r1/3 wide 76 64 1.78 74.36").type == Type::Other);
    CHECK(parseCommandLine("BUFFER lots").type == Type::Other);

    // LF too, and blank lines are dropped.
    lines.clear();
    const char* part3 = "OK\n\r\nPTT OFF\n";
    splitter.feed(part3, std::strlen(part3), lines);
    CHECK_EQ(lines.size(), (size_t)2);
}

void testCommandCallsign()
{
    CHECK_EQ(commandCallsign("vk2abc/p"), std::string("VK2ABC/P"));
    CHECK_EQ(commandCallsign("W1AW-7 "), std::string("W1AW-7"));
    CHECK_EQ(commandCallsign("ABCDEFGHIJKL"), std::string("ABCDEFGHIJ"));
}

void testModes()
{
    const std::vector<ModeInfo>& known = knownModes();
    CHECK(!known.empty());
    for (const ModeInfo& mode : known) CHECK(mode.valid());

    // The 500 Hz setting keeps only the narrow ones, from a host's list or
    // the known one.
    for (const auto& offered : {known, std::vector<ModeInfo>()})
    {
        std::vector<ModeInfo> narrow = narrowModes(offered);
        CHECK(!narrow.empty());
        for (const ModeInfo& mode : narrow) CHECK(mode.bandwidthHz <= NARROW_MODE_MAX_HZ);
    }

    // A burst is one control codeword and the frames behind 2-byte
    // lengths: qpsk-r1/3 carries 76 bytes a codeword.
    ModeInfo qpsk;
    for (const ModeInfo& mode : known)
    {
        if (mode.name == "qpsk-r1/3") qpsk = mode;
    }
    CHECK(qpsk.valid());
    CHECK(std::fabs(qpsk.burstSeconds({74}) - (qpsk.secondsAtOne + (qpsk.secondsAtMax - qpsk.secondsAtOne) / 63)) < 1e-9);
    CHECK(qpsk.burstSeconds({75}) > qpsk.burstSeconds({74}));
    // Never past the most codewords a burst takes.
    CHECK(std::fabs(qpsk.burstSeconds({100000}) - qpsk.secondsAtMax) < 1e-9);
    CHECK(ModeInfo().burstSeconds({10}) == 0.0);
}

} // namespace

int main()
{
    testKissPortsAndAckMode();
    testCommandLines();
    testCommandCallsign();
    testModes();
    return ragtime_test::finish();
}
