#ifndef RAGTIME_GEARSHIFT_ICON_H
#define RAGTIME_GEARSHIFT_ICON_H

#include <wx/bmpbndl.h>
#include <wx/colour.h>

class wxGraphicsContext;

// The Gear button's speedometer, a dial and its needle well up the scale,
// in the theme's text colour. Jeff picked it to stand for picking a speed
// (2026-10-22); the button shows the gear's short name under it.

// Draws the icon in a square size wide at the context's origin.
void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink);

// The icon at 1x and 2x, sizeDip across and three quarters of that tall:
// the empty band under the dial is trimmed off.
wxBitmapBundle gearshiftIcon(const wxColour& ink, int sizeDip = 24);

#endif
