// A broadcast file of theirs not yet whole, as kept on disk so a restart
// doesn't lose it (Jeff, 2026-10-23: "Is the state of the in-progress and
// parts list saved to disk? this should be kept there, too."): its
// announce, the pieces that came, where it is to be saved, and the seals
// heard over it, so a repair can still be asked for after the restart.
//
// One text file a part: a version line, then a line a field, the
// records and pieces in hex, and "end" (a file cut short lacks it). Plain
// enough to read, and nothing in it is trusted on the way back in: a
// piece that doesn't fit the announce is dropped, and a file that
// doesn't parse is ignored.
#ifndef RAGTIME_BROADCAST_BROADCAST_PARTS_H
#define RAGTIME_BROADCAST_BROADCAST_PARTS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ragtime/protocol/Records.h"

namespace ragtime
{
namespace broadcast
{

struct KeptSeal
{
    protocol::Seal seal;
    protocol::Digest digest{};  // the Seal record's
    uint64_t heardMs = 0;
};

struct BroadcastPart
{
    protocol::Record announce;  // the FileAnnounce, header and all
    int state = 0;              // BroadcastFile::State, as a number
    bool stopped = false;
    bool autoReceived = false;
    bool chosenPath = false;
    std::string path;           // UTF-8, where it is to be saved
    uint64_t startedMs = 0;
    uint64_t lastMs = 0;
    uint64_t expiresAtMs = 0;
    std::vector<KeptSeal> seals;
    std::map<uint16_t, std::vector<uint8_t>> pieces;
};

std::string encodePart(const BroadcastPart& part);
bool decodePart(const std::string& text, BroadcastPart& out);

} // namespace broadcast
} // namespace ragtime

#endif
