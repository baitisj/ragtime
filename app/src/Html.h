#ifndef RAGTIME_HTML_H
#define RAGTIME_HTML_H

#include <string>

#include <wx/colour.h>
#include <wx/string.h>

// Small helpers for the lists drawn as HTML (wxHtmlListBox): the messages
// and the narrow station list. Colours come from the system theme, so dark
// mode follows the system.
wxString htmlEscape(const std::string& utf8);
wxString htmlColour(const wxColour& colour);
// a toward b by t.
wxColour blendColour(const wxColour& a, const wxColour& b, double t);
// Secondary text: the theme's text blended toward its background.
wxColour secondaryText();
// Whether the lists' surface is dark: a dark theme.
bool darkSurface();

// Colours for status words and marks, each in a light theme's shade and a
// dark one's, so it reads on either background (Jeff, 2026-10-23: the
// light shades were nearly lost in dark mode).
wxColour goodText();    // green: sent, delivered, saved, available
wxColour badText();     // red: not sent, not delivered, a problem
wxColour quietText();   // amber: the session's link is quiet
wxColour busyText();    // orange: a station busy in a session
wxColour accentText();  // blue: sending, addressed, our own session, links
// The background of a strip that warns the link is quiet.
wxColour quietSurface();

// A link in a row, in accentText() (wxHTML's own is pure blue, lost on a
// dark background), or the selection's text colour when the row is picked.
wxString htmlLink(const wxString& href, const wxString& label, bool picked);

// A row's markup in the theme's list text colour: wxHTML starts in black,
// whatever the theme. A picked row is left to the selection's colours.
wxString listText(const wxString& html, bool picked);

const wxColour ACCENT_BLUE(0x1f, 0x5f, 0xbf);

#endif
