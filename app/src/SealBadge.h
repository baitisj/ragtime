#ifndef RAGTIME_SEAL_BADGE_H
#define RAGTIME_SEAL_BADGE_H

#include <wx/string.h>

#include "ragtime/broadcast/Seals.h"

class wxWindow;

// The seal badge beside a broadcast message (docs/MOCKUPS.md, "Seal state
// icons"): an <img> of the icon for the mark, SEAL_BADGE_PX for window, in the
// light or dark shades as the list is drawn; empty for Mark::None.
//
//   Awaiting   grey clock, no seal         ours: its seal hasn't gone yet
//   Sealed     green check                 ours: the seal went; theirs: it matches their seal
//   Repairing  amber arrow, no seal        ours: a station asked for it, and it goes again
//   Held       green double check          ours: a station's repair request shows it holds it
//   Mismatch   red X                       theirs: what we hold doesn't match their seal
// 20 px: 16 was too small to read (Jeff, 2026-10-23: "another 25% larger").
constexpr int SEAL_BADGE_PX = 20;
wxString sealBadgeHtml(ragtime::broadcast::Seals::Mark mark, const wxWindow* window);

// The same seals for a session message of ours, in place of the words
// (Jeff, 2026-10-24): Delivered is Held's green double check, Not delivered
// a red outlined seal with a slash.
wxString deliveredBadgeHtml(const wxWindow* window);
wxString notDeliveredBadgeHtml(const wxWindow* window);

#endif
