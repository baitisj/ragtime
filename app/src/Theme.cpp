#include "Theme.h"

#include <string>
#include <utility>

#include <wx/event.h>
#include <wx/htmllbox.h>
#include <wx/settings.h>
#include <wx/window.h>

#if defined(__WXGTK3__)
#include <gtk/gtk.h>
#endif

namespace
{

#if defined(__WXGTK3__)
const char* const DARK_SUFFIXES[] = {"-dark", "-Dark"};

// The theme the desktop gave Ragtime, as GTK named it before the first
// switch: some desktops pick a dark theme by name ("Adwaita-dark") rather
// than with the prefer-dark setting.
std::string& desktopTheme()
{
    static std::string name;
    return name;
}

std::string themeName(GtkSettings* settings)
{
    gchar* name = nullptr;
    g_object_get(settings, "gtk-theme-name", &name, nullptr);
    std::string out = name != nullptr ? name : "";
    g_free(name);
    return out;
}

// "Adwaita-dark" is "Adwaita" in the light.
std::string lightVariant(const std::string& name)
{
    for (const char* suffix : DARK_SUFFIXES)
    {
        size_t at = name.find(suffix);
        if (at != std::string::npos) return name.substr(0, at) + name.substr(at + std::string(suffix).size());
    }
    return name;
}

// As wx does when the desktop's colour scheme changes: each window passes
// it on to its children and repaints.
void tellWindows()
{
    for (wxWindowList::compatibility_iterator node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext())
    {
        wxWindow* win = node->GetData();
        wxSysColourChangedEvent event;
        event.SetEventObject(win);
        win->HandleWindowEvent(event);
    }
}
#endif

} // namespace

bool canSwitchTheme()
{
#if defined(__WXGTK3__)
    return true;
#else
    return false;
#endif
}

bool themeIsDark()
{
    return wxSystemSettings::GetAppearance().IsDark();
}

void applyTheme(ThemeChoice choice)
{
#if defined(__WXGTK3__)
    if (choice == ThemeChoice::System) return;
    GtkSettings* settings = gtk_settings_get_default();
    if (settings == nullptr) return;
    if (desktopTheme().empty()) desktopTheme() = themeName(settings);
    const bool dark = choice == ThemeChoice::Dark;
    const std::string wanted = dark ? desktopTheme() : lightVariant(desktopTheme());
    if (themeName(settings) != wanted) g_object_set(settings, "gtk-theme-name", wanted.c_str(), nullptr);
    g_object_set(settings, "gtk-application-prefer-dark-theme", dark ? TRUE : FALSE, nullptr);
    // wx keeps the theme's colours until it hears the theme's name change.
    g_object_notify(G_OBJECT(settings), "gtk-theme-name");
    // A theme with no dark variant, or one named but not installed (GTK
    // falls back to Adwaita's light), stays as it was: then Adwaita, built
    // into GTK, has both.
    if (themeIsDark() != dark && themeName(settings) != "Adwaita")
        g_object_set(settings, "gtk-theme-name", "Adwaita", nullptr);
    tellWindows();
#else
    (void)choice;
#endif
}

void followThemeBackground(wxWindow* window, wxSystemColour colour)
{
    window->SetBackgroundColour(wxSystemSettings::GetColour(colour));
    window->Bind(wxEVT_SYS_COLOUR_CHANGED, [window, colour](wxSysColourChangedEvent& e) {
        e.Skip();
        window->SetBackgroundColour(wxSystemSettings::GetColour(colour));
        if (auto* list = wxDynamicCast(window, wxHtmlListBox)) list->RefreshAll();
        window->Refresh();
    });
}

void followThemeText(wxWindow* window, std::function<wxColour()> colour)
{
    window->SetForegroundColour(colour());
    window->Bind(wxEVT_SYS_COLOUR_CHANGED, [window, colour = std::move(colour)](wxSysColourChangedEvent& e) {
        e.Skip();
        window->SetForegroundColour(colour());
        window->Refresh();
    });
}
