// What broadcasts cost on the air, in Data2G's own terms: every timing in
// broadcast files and repair is a number of burst airtimes in a Data2G
// mode, from the host's MODES list (docs/PROTOCOL.md, "Seals and
// repair"), never a Glissando tempo. Also how records are packed into
// frames, and frames into bursts.
//
// Some of this follows Glissando's group files (Data2GBroadcast, written
// there for Glissando and relicensed MIT by Jeff): pieces filling whole
// codewords, one frame a burst in the FSK modes, up to eight in the OFDM
// ones.
#ifndef RAGTIME_BROADCAST_AIRTIME_H
#define RAGTIME_BROADCAST_AIRTIME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/link/Data2GLink.h"
#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

// A burst written to the host is one transmission: at most this many
// frames, and at most this long, in the modes that take several.
constexpr int MAX_FRAMES_PER_BURST = 8;
constexpr double MAX_BURST_SECONDS = 12.0;

// What a burst is taken to cost when the mode isn't known: a
// one-codeword burst in the slowest mode Glissando met on the air.
constexpr double UNKNOWN_BURST_SECONDS = 16.0;

// The mode called name among those offered (the known ones while the host
// has listed none). An empty name means the group's mode as it stands,
// groupMode. Not valid() if neither is there.
link::ModeInfo findMode(const std::vector<link::ModeInfo>& offered, const std::string& name,
                        const std::string& groupMode = std::string());

// Data2G's CPM modes (the FSK ones) carry one frame a burst in practice;
// its OFDM modes put several in one.
bool isCpmMode(const link::ModeInfo& mode);

// The airtime of a burst of one codeword of data (with its control
// codeword): what a short burst such as a repair request costs.
double oneCodewordSeconds(const link::ModeInfo& mode);

// The listening window after a seal: 3 one-codeword bursts, plus 4 s for
// the channel to turn round (docs/PLAN.md, "Windows in Data2G terms").
constexpr double WINDOW_BURSTS = 3.0;
constexpr uint64_t WINDOW_TURNAROUND_MS = 4000;
uint64_t listeningWindowMs(const link::ModeInfo& mode);

// The frame a file's pieces fill in a mode: whole codewords less Data2G's
// 2-byte length (3 codewords in the FSK modes, as many as fit the frame
// limit in the others), at most MAX_BROADCAST_FRAME.
int pieceFrameBytes(const link::ModeInfo& mode);

// The bytes of a file each FilePiece carries, for a sender with this
// callsign, so its frame (FRAME_BYTE and the record) fills
// pieceFrameBytes. At least 16.
int pieceBytesFor(const link::ModeInfo& mode, const std::string& callsign);

// Records into frames of at most MAX_BROADCAST_FRAME, in order, as many to
// a frame as fit. A record that can't be coded is left out.
std::vector<std::vector<uint8_t>> packFrames(const std::vector<protocol::Record>& records);

// Frames of these lengths can go as one burst: one frame in the FSK modes
// (and when the mode isn't known); otherwise as many as fit the mode's
// codewords, MAX_FRAMES_PER_BURST and MAX_BURST_SECONDS.
bool fitsOneBurst(const std::vector<int>& frameBytes, const link::ModeInfo& mode);

// Frames into bursts, in order, each as full as fitsOneBurst allows.
std::vector<std::vector<std::vector<uint8_t>>> packBursts(const std::vector<std::vector<uint8_t>>& frames,
                                                         const link::ModeInfo& mode);

// The airtime of a burst of these frames; UNKNOWN_BURST_SECONDS for each
// codeword's worth when the mode isn't known.
double burstSeconds(const std::vector<std::vector<uint8_t>>& frames, const link::ModeInfo& mode);

// A file's announce goes again ahead of every this many pieces, so a
// station that tunes in part way knows what it is hearing.
constexpr int ANNOUNCE_EVERY_PIECES = 20;
// A file's bursts leave the channel this long between them, so other
// stations can get a word in.
constexpr uint64_t FILE_BURST_GAP_MS = 3000;
// The longest a seal stays open for repairs unless the operator keeps it
// open longer.
constexpr uint64_t MAX_OPEN_MS = 2 * 60 * 60 * 1000;

// What broadcasting a file takes, for the operator to agree to before it
// goes: its pieces, the first pass's airtime (announces and pieces), that
// with the gaps between bursts, and how long repairs stay open at most.
struct FileEstimate
{
    int pieceBytes = 0;
    int pieces = 0;
    int bursts = 0;
    double airSeconds = 0.0;
    double wallSeconds = 0.0;
    uint64_t repairsMs = 0;
};
FileEstimate estimateFile(uint64_t size, const std::string& name, const std::string& callsign,
                          const link::ModeInfo& mode);

} // namespace broadcast
} // namespace ragtime

#endif
