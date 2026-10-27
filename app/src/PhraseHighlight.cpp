#include "PhraseHighlight.h"

#include <wx/settings.h>

#if defined(__WXGTK3__)
#include <gtk/gtk.h>
#endif

#include "Html.h"

namespace
{

// The faint green: speedy and good (Chris's suggestion, 2026-10-27; it was
// Glissando's red), as a tint of the line's own background so it reads in
// light and dark themes.
wxColour phraseColour(const wxColour& background)
{
    return blendColour(background, wxColour(0x2e, 0xa0, 0x4a), 0.22);
}

} // namespace

void highlightPhrases(wxTextCtrl* entry, const std::string& utf8,
                      const std::vector<ragtime::text::HamText::PhraseSpan>& spans)
{
    const wxColour tint = phraseColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#if defined(__WXGTK3__)
    // A single-line wxTextCtrl is a GtkEntry. Pango counts in bytes of the
    // UTF-8 text, as the spans do.
    GtkEntry* gtkEntry = reinterpret_cast<GtkEntry*>(entry->GetHandle());
    if (gtkEntry == nullptr || !GTK_IS_ENTRY(gtkEntry)) return;
    PangoAttrList* list = pango_attr_list_new();
    for (const auto& span : spans)
    {
        if (span.start + span.length > utf8.size()) continue;
        PangoAttribute* attr = pango_attr_background_new((guint16)(tint.Red() * 257), (guint16)(tint.Green() * 257),
                                                         (guint16)(tint.Blue() * 257));
        attr->start_index = (guint)span.start;
        attr->end_index = (guint)(span.start + span.length);
        pango_attr_list_insert(list, attr);
    }
    gtk_entry_set_attributes(gtkEntry, list);
    pango_attr_list_unref(list);
#else
    // Positions count the line's own characters, not bytes.
    auto position = [&utf8](size_t byte) { return (long)wxString::FromUTF8(utf8.data(), byte).length(); };
    wxTextAttr plain;
    plain.SetBackgroundColour(entry->GetBackgroundColour());
    entry->SetStyle(0, entry->GetLastPosition(), plain);
    wxTextAttr phrase;
    phrase.SetBackgroundColour(tint);
    for (const auto& span : spans)
    {
        if (span.start + span.length > utf8.size()) continue;
        entry->SetStyle(position(span.start), position(span.start + span.length), phrase);
    }
#endif
}
