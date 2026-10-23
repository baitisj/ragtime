#ifndef RAGTIME_DRAWN_ICON_H
#define RAGTIME_DRAWN_ICON_H

#include <functional>

#include <wx/bmpbndl.h>
#include <wx/colour.h>

class wxGraphicsContext;

// The small button icons Ragtime draws itself, in the theme's text colour
// so they follow light and dark: the gearshift's speedometer and the file
// buttons' floppy disk.

// Draws an icon in a square size wide at the context's origin.
using DrawIcon = std::function<void(wxGraphicsContext& gc, double size)>;

// The icon at 1x and 2x, sizeDip across.
wxBitmapBundle drawnIcon(const DrawIcon& draw, int sizeDip);

// A floppy disk, for the buttons that send a file (Jeff, 2026-10-23).
void drawFloppyIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle floppyIcon(const wxColour& ink, int sizeDip = 18);

#endif
