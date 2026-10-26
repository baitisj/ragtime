#ifndef RAGTIME_REACTION_CHIPS_H
#define RAGTIME_REACTION_CHIPS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/string.h>

#include "ragtime/protocol/Reactions.h"

class wxMenu;

// Reactions as the chat tabs show them (docs/PROTOCOL.md, "Reactions"):
// chips under a line, "👍 KC2G, W1AW   73 AG7EW", and the React submenu
// on its right-click menu.

// The chips' markup, for under a line's text; empty with none. A chip
// that clicking adds ours to (or, ours still waiting to go and
// canTakeBack, takes it back) is a link "react:<code>"; ours is in bold,
// and in italics while it waits to go.
wxString reactionChipsHtml(const std::vector<ragtime::protocol::Reacted>& theirs, uint8_t mine, uint8_t waiting,
                           const std::string& me, bool canReact, bool canTakeBack, bool picked);

// "react:<code>": the code, or 0 for another href.
uint8_t reactionHref(const wxString& href);

// The React submenu: the palette, ours ticked, with ids from firstId on
// (REACTION_MENU_IDS of them at most), and "Take back mine" when ours is
// still waiting and canTakeBack. pick gets the code chosen, or 0 for take
// back.
constexpr int REACTION_MENU_IDS = 40;
wxMenu* reactionMenu(int firstId, uint8_t current, bool canTakeBack, std::function<void(uint8_t code)> pick);

// "W1AW reacted 👍 (thumbs up) to your line: CQ CQ...": for the status bar.
std::string reactionNote(const std::string& from, uint8_t code, const std::string& line);

#endif
