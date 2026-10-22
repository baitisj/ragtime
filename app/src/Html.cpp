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

wxColour secondaryText()
{
    return blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT),
                       wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX), 0.4);
}
