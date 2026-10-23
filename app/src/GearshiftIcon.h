#ifndef RAGTIME_GEARSHIFT_ICON_H
#define RAGTIME_GEARSHIFT_ICON_H

#include <wx/bmpbndl.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/string.h>

class wxGraphicsContext;

// The Gear button's speedometer, a dial and its needle well up the scale,
// in the theme's text colour. Jeff picked it to stand for picking a speed
// (2026-10-22).

// Draws the dial in a square size wide at the context's origin.
void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink);

// The Gear button's picture at 1x and 2x: the dial, dialDip across, with
// the gear's short name ("A29") in font close under it.
wxBitmapBundle gearButtonIcon(const wxColour& ink, const wxFont& font, const wxString& name, int dialDip);

#endif
