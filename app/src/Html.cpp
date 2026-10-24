#include "Html.h"

#include <cmath>

#include <wx/settings.h>

wxString htmlEscape(const std::string& utf8)
{
    wxString text = wxString::FromUTF8(utf8);
    wxString out;
    out.reserve(text.length());
    for (wxUniChar c : text)
    {
        switch ((int)c.GetValue())
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\n':
                out += "<br>";
                break;
            default:
                out += c;
        }
    }
    return out;
}

wxString htmlColour(const wxColour& colour)
{
    return colour.GetAsString(wxC2S_HTML_SYNTAX);
}

wxColour blendColour(const wxColour& a, const wxColour& b, double t)
{
    auto mix = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(std::lround(x + (y - x) * t));
    };
    return wxColour(mix(a.Red(), b.Red()), mix(a.Green(), b.Green()), mix(a.Blue(), b.Blue()));
}

bool darkSurface()
{
    return wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX).GetLuminance() < 0.5;
}

namespace
{

wxColour shade(const wxColour& light, const wxColour& dark)
{
    return darkSurface() ? dark : light;
}

} // namespace

wxColour goodText()
{
    return shade(wxColour(0x1e, 0x6b, 0x34), wxColour(0x6c, 0xd4, 0x86));
}

wxColour badText()
{
    return shade(wxColour(0xb4, 0x23, 0x18), wxColour(0xff, 0x86, 0x78));
}

wxColour quietText()
{
    return shade(wxColour(0x8a, 0x4b, 0x00), wxColour(0xff, 0xcf, 0x5c));
}

wxColour busyText()
{
    return shade(wxColour(0xb5, 0x47, 0x08), wxColour(0xff, 0xa8, 0x5c));
}

wxColour needleOrange()
{
    return shade(wxColour(0xe8, 0x6a, 0x10), wxColour(0xff, 0x9a, 0x3c));
}

wxColour accentText()
{
    return shade(ACCENT_BLUE, wxColour(0x86, 0xb6, 0xff));
}

wxColour quietSurface()
{
    // Amber, faint enough on a dark surface that the light text reads.
    return blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX), wxColour(0xf7, 0xc6, 0x00),
                       darkSurface() ? 0.16 : 0.30);
}

wxString htmlLink(const wxString& href, const wxString& label, bool picked)
{
    wxColour ink = picked ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT) : accentText();
    return "<a href=\"" + href + "\"><font color=\"" + htmlColour(ink) + "\">" + label + "</font></a>";
}

wxString listText(const wxString& html, bool picked)
{
    if (picked) return html;
    return "<font color=\"" + htmlColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT)) + "\">" + html + "</font>";
}

wxColour secondaryText()
{
    return blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT),
                       wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX), 0.4);
}
