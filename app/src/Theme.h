#ifndef RAGTIME_THEME_H
#define RAGTIME_THEME_H

#include <functional>

#include <wx/colour.h>
#include <wx/settings.h>

class wxWindow;

// Light or dark, from the status bar's button (Jeff, 2026-10-23). Ragtime
// keeps the system theme and its standard widgets, so on GTK the switch is
// the theme's own dark variant (GtkSettings'
// gtk-application-prefer-dark-theme), and every window hears
// wxEVT_SYS_COLOUR_CHANGED, as it does when the desktop switches. wxWidgets
// 3.2 has no dark mode for Windows, so there the button isn't shown.

enum class ThemeChoice
{
    System, // as the desktop has it, until the operator picks
    Light,
    Dark,
};

// Whether this build can switch.
bool canSwitchTheme();

// Whether the theme now showing is dark.
bool themeIsDark();

// Switches to the choice, and tells every window when the colours change.
// System leaves the theme as it is.
void applyTheme(ThemeChoice choice);

// Colours a window was given when it was made (a system colour for its
// background, a blend for its text) stay put when the theme changes:
// these give them again then. An html list also builds its rows anew, as
// their markup has colours in it.
void followThemeBackground(wxWindow* window, wxSystemColour colour);
void followThemeText(wxWindow* window, std::function<wxColour()> colour);

#endif
