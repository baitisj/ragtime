#include "ragtime/protocol/Reactions.h"

#include <algorithm>

namespace ragtime
{
namespace protocol
{

const std::vector<ReactionEntry>& reactionPalette()
{
    // Jeff, 2026-10-26: twelve emoji, then ham words shown as chips.
    static const std::vector<ReactionEntry> palette = {
        {1, "\xF0\x9F\x91\x8D", "thumbs up", false},
        {2, "\xE2\x9D\xA4\xEF\xB8\x8F", "heart", false},
        {3, "\xF0\x9F\x98\x82", "laughing", false},
        {4, "\xF0\x9F\x98\xAE", "wow", false},
        {5, "\xF0\x9F\x98\xA2", "sad", false},
        {6, "\xF0\x9F\x8E\x89", "hooray", false},
        {7, "\xF0\x9F\x91\x8F", "applause", false},
        {8, "\xF0\x9F\x99\x8F", "thanks", false},
        {9, "\xF0\x9F\x91\x8B", "wave", false},
        {10, "\xE2\x98\x95", "coffee", false},
        {11, "\xF0\x9F\x93\xBB", "radio", false},
        {12, "\xE2\x9A\xA1", "lightning", false},
        {13, "73", "best regards", true},
        {14, "88", "love and kisses", true},
        {15, "R", "roger, received", true},
        {16, "QSL", "I confirm", true},
        {17, "TU", "thank you", true},
        {18, "FB", "fine business", true},
        {19, "HI HI", "laughing, in CW", true},
        {20, "QRZ?", "who is calling?", true},
    };
    return palette;
}

const ReactionEntry* reactionEntry(uint8_t code)
{
    for (const ReactionEntry& e : reactionPalette())
    {
        if (e.code == code) return &e;
    }
    return nullptr;
}

std::string reactionShown(uint8_t code)
{
    const ReactionEntry* e = reactionEntry(code);
    return e != nullptr ? std::string(e->shown) : std::string("?");
}

bool setReaction(std::vector<Reacted>& reactions, const std::string& call, uint8_t code)
{
    for (auto it = reactions.begin(); it != reactions.end(); ++it)
    {
        if (it->call != call) continue;
        if (it->code == code) return false;
        // The new one goes last, as the newest heard.
        reactions.erase(it);
        break;
    }
    reactions.push_back(Reacted{call, code});
    return true;
}

std::vector<ReactionChip> reactionChips(const std::vector<Reacted>& theirs, uint8_t mine, uint8_t myWaiting,
                                        const std::string& me)
{
    std::vector<ReactionChip> chips;
    auto chipFor = [&chips](uint8_t code) -> ReactionChip& {
        for (ReactionChip& c : chips)
        {
            if (c.code == code) return c;
        }
        chips.push_back(ReactionChip());
        chips.back().code = code;
        return chips.back();
    };
    for (const Reacted& r : theirs)
    {
        if (r.code == 0 || r.call == me) continue;
        chipFor(r.code).calls.push_back(r.call);
    }
    uint8_t ours = myWaiting != 0 ? myWaiting : mine;
    if (ours != 0)
    {
        ReactionChip& c = chipFor(ours);
        c.calls.push_back(me);
        c.mine = true;
        c.waiting = myWaiting != 0;
    }
    // Palette order, so a chip doesn't hop about as reactions come in.
    auto rank = [](uint8_t code) -> int {
        const auto& palette = reactionPalette();
        for (size_t i = 0; i < palette.size(); i++)
        {
            if (palette[i].code == code) return (int)i;
        }
        return 256 + code;
    };
    std::stable_sort(chips.begin(), chips.end(),
                     [&rank](const ReactionChip& a, const ReactionChip& b) { return rank(a.code) < rank(b.code); });
    return chips;
}

} // namespace protocol
} // namespace ragtime
