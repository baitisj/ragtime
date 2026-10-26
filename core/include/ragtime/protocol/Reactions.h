// Reactions (docs/PROTOCOL.md, "Reactions"): the published palette a
// Reaction record's one byte names, and the reactions a line has gathered,
// one per station, as the chat tabs show them.
//
// The palette is a protocol constant, like text table 0: a code once given
// never changes its meaning. New entries go on the end. A code this version
// doesn't know is kept and shown as "?".
#ifndef RAGTIME_PROTOCOL_REACTIONS_H
#define RAGTIME_PROTOCOL_REACTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace protocol
{

struct ReactionEntry
{
    uint8_t code;
    const char* shown;      // the UTF-8 shown: "👍", or a ham word, "73"
    const char* name;       // in words, for menus and tooltips: "thumbs up", "best regards"
    bool word;              // a ham word rather than an emoji
};

// Every entry, in the order menus list them.
const std::vector<ReactionEntry>& reactionPalette();
// The entry for code, or nullptr for one this version doesn't know (0
// included).
const ReactionEntry* reactionEntry(uint8_t code);
// What the code shows: its emoji or word, or "?" for one unknown.
std::string reactionShown(uint8_t code);

// A station's reaction to a line.
struct Reacted
{
    std::string call;
    uint8_t code = 0;
};

// One reaction per station: call's replaces the one it gave before. False
// when it is the same as before.
bool setReaction(std::vector<Reacted>& reactions, const std::string& call, uint8_t code);

// A line's reactions, gathered by code for the chips under it: the code,
// the stations, ours among them. Ours is shown as the one waiting to go
// when there is one, else the one sent. In palette order, then unknown
// codes by number.
struct ReactionChip
{
    uint8_t code = 0;
    std::vector<std::string> calls;     // in the order heard, ours last
    bool mine = false;
    bool waiting = false;               // ours, not yet on the air
};
std::vector<ReactionChip> reactionChips(const std::vector<Reacted>& theirs, uint8_t mine, uint8_t myWaiting,
                                        const std::string& me);

} // namespace protocol
} // namespace ragtime

#endif
