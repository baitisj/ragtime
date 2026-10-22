#include "ragtime/broadcast/Airtime.h"

#include <algorithm>
#include <cmath>

namespace ragtime
{
namespace broadcast
{

namespace
{

// Data2G puts a 2-byte length ahead of each frame in a burst.
constexpr int LENGTH_BYTES = 2;
constexpr int MIN_PIECE_BYTES = 16;

int codewordsFor(const std::vector<int>& frameBytes, const link::ModeInfo& mode)
{
    int bytes = 0;
    for (int length : frameBytes) bytes += LENGTH_BYTES + length;
    return 1 + (bytes + mode.bytesPerCodeword - 1) / mode.bytesPerCodeword;
}

} // namespace

link::ModeInfo findMode(const std::vector<link::ModeInfo>& offered, const std::string& name,
                        const std::string& groupMode)
{
    const std::vector<link::ModeInfo>& modes = offered.empty() ? link::knownModes() : offered;
    const std::string& wanted = name.empty() ? groupMode : name;
    for (const link::ModeInfo& m : modes)
    {
        if (!wanted.empty() && m.name == wanted) return m;
    }
    return link::ModeInfo();
}

bool isCpmMode(const link::ModeInfo& mode)
{
    return mode.name.compare(0, 3, "fsk") == 0;
}

double oneCodewordSeconds(const link::ModeInfo& mode)
{
    if (!mode.valid()) return UNKNOWN_BURST_SECONDS;
    return mode.burstSeconds({std::max(1, mode.bytesPerCodeword - LENGTH_BYTES)});
}

uint64_t listeningWindowMs(const link::ModeInfo& mode)
{
    return (uint64_t)std::ceil(WINDOW_BURSTS * oneCodewordSeconds(mode) * 1000.0) + WINDOW_TURNAROUND_MS;
}

int pieceFrameBytes(const link::ModeInfo& mode)
{
    const int limit = (int)protocol::MAX_BROADCAST_FRAME;
    int pb = mode.bytesPerCodeword;
    if (!mode.valid() || pb - LENGTH_BYTES >= limit) return limit;
    // Every codeword but the burst's control one may carry data.
    int most = std::max(1, mode.maxCodewords - 1);
    int k = isCpmMode(mode) ? 3 : (limit + LENGTH_BYTES) / pb;
    k = std::min(std::max(k, 1), most);
    while (k > 1 && k * pb - LENGTH_BYTES > limit) k--;
    return std::max(1, std::min(k * pb - LENGTH_BYTES, limit));
}

int pieceBytesFor(const link::ModeInfo& mode, const std::string& callsign)
{
    int frame = pieceFrameBytes(mode);
    protocol::Record r;
    r.header.callsign = protocol::normalCallsign(callsign);
    if (r.header.callsign.empty()) r.header.callsign = "N0CALL";
    protocol::FilePiece piece;
    r.body = piece;
    // FRAME_BYTE, then the record's outline; its length field may take a
    // second byte once the piece is long.
    int overhead = 1 + (int)protocol::encodeRecord(r).size();
    int bytes = std::max(MIN_PIECE_BYTES, frame - overhead);
    while (bytes > MIN_PIECE_BYTES)
    {
        piece.bytes.assign((size_t)bytes, 0x55);
        r.body = piece;
        if (1 + (int)protocol::encodeRecord(r).size() <= frame) break;
        bytes--;
    }
    return bytes;
}

std::vector<std::vector<uint8_t>> packFrames(const std::vector<protocol::Record>& records)
{
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> frame{protocol::FRAME_BYTE};
    for (const protocol::Record& r : records)
    {
        std::vector<uint8_t> bytes = protocol::encodeRecord(r);
        if (bytes.empty() || bytes.size() + 1 > protocol::MAX_BROADCAST_FRAME) continue;
        if (frame.size() + bytes.size() > protocol::MAX_BROADCAST_FRAME)
        {
            frames.push_back(std::move(frame));
            frame.assign(1, protocol::FRAME_BYTE);
        }
        frame.insert(frame.end(), bytes.begin(), bytes.end());
    }
    if (frame.size() > 1) frames.push_back(std::move(frame));
    return frames;
}

bool fitsOneBurst(const std::vector<int>& frameBytes, const link::ModeInfo& mode)
{
    if (frameBytes.size() <= 1) return true;
    if (!mode.valid() || isCpmMode(mode)) return false;
    if ((int)frameBytes.size() > MAX_FRAMES_PER_BURST) return false;
    if (codewordsFor(frameBytes, mode) > mode.maxCodewords) return false;
    return mode.burstSeconds(frameBytes) <= MAX_BURST_SECONDS;
}

std::vector<std::vector<std::vector<uint8_t>>> packBursts(const std::vector<std::vector<uint8_t>>& frames,
                                                         const link::ModeInfo& mode)
{
    std::vector<std::vector<std::vector<uint8_t>>> bursts;
    std::vector<int> lengths;
    for (const std::vector<uint8_t>& frame : frames)
    {
        lengths.push_back((int)frame.size());
        if (bursts.empty() || !fitsOneBurst(lengths, mode))
        {
            bursts.emplace_back();
            lengths.assign(1, (int)frame.size());
        }
        bursts.back().push_back(frame);
    }
    return bursts;
}

double burstSeconds(const std::vector<std::vector<uint8_t>>& frames, const link::ModeInfo& mode)
{
    std::vector<int> lengths;
    for (const std::vector<uint8_t>& frame : frames) lengths.push_back((int)frame.size());
    if (mode.valid()) return mode.burstSeconds(lengths);
    return UNKNOWN_BURST_SECONDS * (double)std::max<size_t>(1, frames.size());
}

FileEstimate estimateFile(uint64_t size, const std::string& name, const std::string& callsign,
                          const link::ModeInfo& mode)
{
    FileEstimate estimate;
    estimate.pieceBytes = pieceBytesFor(mode, callsign);
    estimate.pieces = (int)((size + (uint64_t)estimate.pieceBytes - 1) / (uint64_t)estimate.pieceBytes);
    estimate.repairsMs = MAX_OPEN_MS;

    protocol::Record announce;
    announce.header.callsign = protocol::normalCallsign(callsign);
    if (announce.header.callsign.empty()) announce.header.callsign = "N0CALL";
    protocol::FileAnnounce a;
    a.size = (uint32_t)size;
    a.pieceBytes = (uint16_t)estimate.pieceBytes;
    a.name = name.substr(0, protocol::MAX_FILE_NAME);
    announce.body = a;
    int announceFrame = 1 + (int)protocol::encodeRecord(announce).size();
    int pieceFrame = pieceFrameBytes(mode);

    // The bursts as the sender makes them: an announce ahead of each block
    // of pieces, the frames packed into bursts as they fit.
    std::vector<int> burst;
    auto close = [&]() {
        if (burst.empty()) return;
        std::vector<std::vector<uint8_t>> frames;
        for (int length : burst) frames.emplace_back((size_t)length, 0);
        estimate.airSeconds += burstSeconds(frames, mode);
        estimate.bursts++;
        burst.clear();
    };
    auto add = [&](int length) {
        burst.push_back(length);
        if (!fitsOneBurst(burst, mode))
        {
            burst.pop_back();
            close();
            burst.push_back(length);
        }
    };
    for (int i = 0; i < estimate.pieces; i++)
    {
        if (i % ANNOUNCE_EVERY_PIECES == 0) add(announceFrame);
        uint64_t left = size - (uint64_t)i * (uint64_t)estimate.pieceBytes;
        int bytes = (int)std::min<uint64_t>(left, (uint64_t)estimate.pieceBytes);
        add(bytes == estimate.pieceBytes ? pieceFrame : pieceFrame - (estimate.pieceBytes - bytes));
    }
    close();
    estimate.wallSeconds = estimate.airSeconds
                           + (double)std::max(0, estimate.bursts - 1) * ((double)FILE_BURST_GAP_MS / 1000.0);
    return estimate;
}

} // namespace broadcast
} // namespace ragtime
