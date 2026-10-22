#include "AppIcon.h"

#include <wx/icon.h>

#ifndef __WXMSW__
#include <wx/image.h>
#include <wx/mstream.h>

#include "RagtimeIconPng.h"
#endif

wxIconBundle appIcons()
{
#ifdef __WXMSW__
    // Every size in ragtime.ico, from app/res/ragtime.rc.
    return wxIconBundle("appicon", nullptr);
#else
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
        wxImage::AddHandler(new wxPNGHandler);
    wxMemoryInputStream in(RAGTIME_ICON_PNG, sizeof RAGTIME_ICON_PNG);
    const wxImage image(in, wxBITMAP_TYPE_PNG);
    wxIconBundle icons;
    if (!image.IsOk())
        return icons;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
    {
        wxIcon icon;
        icon.CopyFromBitmap(wxBitmap(image.Scale(size, size, wxIMAGE_QUALITY_HIGH)));
        icons.AddIcon(icon);
    }
    return icons;
#endif
}
