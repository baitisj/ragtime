#ifndef RAGTIME_GEARSHIFT_ICON_H
#define RAGTIME_GEARSHIFT_ICON_H

#include <wx/bmpbndl.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/string.h>

class wxGraphicsContext;

// The Gear button's speedometer, a dial in the theme's text colour with an
// orange needle pointing at the gear's speed (Jeff, 2026-10-24). Jeff
// picked it to stand for picking a speed (2026-10-22).

// Draws the dial in a square size wide at the context's origin, the needle
// needleAt of the way round it (0 the left end, 1 the right;
// gearbox::dialPosition gives it for a speed).
void drawGearshiftIcon(wxGraphicsContext& gc, double size, const wxColour& ink, const wxColour& needleInk,
                       double needleAt);

// The Gear button's picture at 1x and 2x: the dial, dialDip across, with
// the gear's short name ("A29") in font close under it.
wxBitmapBundle gearButtonIcon(const wxColour& ink, const wxColour& needleInk, double needleAt, const wxFont& font,
                              const wxString& name, int dialDip);

#endif
