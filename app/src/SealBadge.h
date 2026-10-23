#ifndef RAGTIME_SEAL_BADGE_H
#define RAGTIME_SEAL_BADGE_H

#include <wx/string.h>

#include "ragtime/broadcast/Seals.h"

class wxWindow;

// The seal badge beside a broadcast message (docs/MOCKUPS.md, "Seal state
// icons"): an <img> of the icon for the mark, 16 px for window, in the
// light or dark shades as the list is drawn; empty for Mark::None.
//
//   Awaiting   grey, dashed, clock         ours: its seal hasn't gone yet
//   Sealed     green check                 ours: the seal went; theirs: it matches their seal
//   Repairing  amber, dashed, turning      ours: a station asked for it, and it goes again
//   Held       green double check          ours: a station's repair request shows it holds it
//   Mismatch   red X                       theirs: what we hold doesn't match their seal
wxString sealBadgeHtml(ragtime::broadcast::Seals::Mark mark, const wxWindow* window);

#endif
