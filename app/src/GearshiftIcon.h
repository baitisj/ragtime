#ifndef RAGTIME_GEARSHIFT_ICON_H
#define RAGTIME_GEARSHIFT_ICON_H

#include <wx/bmpbndl.h>
#include <wx/colour.h>

class wxGraphicsContext;

// The connection bar's gearshift button: a speedometer, a dial and its
// needle well up the scale, in the theme's text colour. Jeff asked for an
// icon that stands for picking a speed, to save room (2026-10-22).

// Draws the icon in a square size wide at the context's origin.
void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink);

// The icon at 1x and 2x, sizeDip across.
wxBitmapBundle gearshiftIcon(const wxColour& ink, int sizeDip = 20);

#endif
