#ifndef RAGTIME_PHRASE_HIGHLIGHT_H
#define RAGTIME_PHRASE_HIGHLIGHT_H

#include <string>
#include <vector>

#include <wx/textctrl.h>

#include "ragtime/text/HamText.h"

// The ham phrases text table 0 codes as one symbol each ("CQ CQ", " the",
// "73", tools/ham_table/phrases.txt) get a faint red background in an entry
// line as they are typed, as in Glissando's COMMS entry box, so the
// operator sees what rides accelerated.
//
// utf8 is the line's text as sent and spans are byte spans of it. A GtkEntry
// takes no wx text styles, so on GTK the spans go to it as Pango attributes;
// elsewhere the line needs wxTE_RICH2 (PHRASE_ENTRY_STYLE).
void highlightPhrases(wxTextCtrl* entry, const std::string& utf8,
                      const std::vector<ragtime::text::HamText::PhraseSpan>& spans);

// The style an entry line needs for highlightPhrases(), beside its own.
#ifdef __WXMSW__
constexpr long PHRASE_ENTRY_STYLE = wxTE_RICH2;
#else
constexpr long PHRASE_ENTRY_STYLE = 0;
#endif

#endif
