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

const wxColour ACCENT_BLUE(0x1f, 0x5f, 0xbf);

#endif
