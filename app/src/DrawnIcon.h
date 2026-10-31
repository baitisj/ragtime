#ifndef RAGTIME_DRAWN_ICON_H
#define RAGTIME_DRAWN_ICON_H

#include <functional>

#include <wx/bmpbndl.h>
#include <wx/colour.h>
#include <wx/string.h>

class wxGraphicsContext;
class wxWindow;

// The small button icons Ragtime draws itself, in the theme's text colour
// so they follow light and dark: the gearshift's speedometer and the file
// buttons' floppy disk and the picture buttons' framed picture, the status bar's moon and sun, the green
// check on Done, a watched station's bell, the Ear's ear, and the microphone deck's microphone.

// Draws an icon in a square size wide at the context's origin.
using IconDrawing = std::function<void(wxGraphicsContext& gc, double size)>;

// The icon at 1x and 2x, sizeDip across.
wxBitmapBundle drawnIcon(const IconDrawing& draw, int sizeDip);

// A floppy disk, for the buttons that send a file (Jeff, 2026-10-23).
void drawFloppyIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle floppyIcon(const wxColour& ink, int sizeDip = 18);

// A framed picture of hills under the sun, for the buttons that send a
// picture inline (Jeff, 2026-10-27: just left of the floppy disk).
void drawPictureIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle pictureIcon(const wxColour& ink, int sizeDip = 18);

// The light and dark button: a crescent moon in the light (click for
// dark), a sun in the dark (click for light).
void drawMoonIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
void drawSunIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle themeIcon(bool dark, const wxColour& ink, int sizeDip = 16);

// A check mark, for Done on a dialog whose changes already took effect
// (Jeff, 2026-10-25: "X Close" read as cancelling them). Drawn in the
// colour given, goodText() green on the gearshift.
void drawCheckIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle checkIcon(const wxColour& ink, int sizeDip = 16);

// A bell, on a watched station's row (the watch list, and Tell me when
// free).
void drawBellIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle bellIcon(const wxColour& ink, int sizeDip = 14);
// The bell as an <img> for a wxHTML list's row, drawn once for each ink
// and size into wxHTML's memory files; "" if it couldn't be drawn.
wxString bellHtml(const wxWindow* window, int sizeDip, const wxColour& ink);

// An ear, on the connection bar's button that opens the Ear (what the
// radio hears).
void drawEarIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle earIcon(const wxColour& ink, int sizeDip = 18);

// A microphone, on the buttons that open the microphone deck (voice
// messages), beside the picture buttons.
void drawMicrophoneIcon(wxGraphicsContext& gc, double size, const wxColour& ink);
wxBitmapBundle microphoneIcon(const wxColour& ink, int sizeDip = 18);

#endif
