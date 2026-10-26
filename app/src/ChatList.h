#ifndef RAGTIME_CHAT_LIST_H
#define RAGTIME_CHAT_LIST_H

#include <cstddef>

#include <wx/htmllbox.h>

// A chat's messages as HTML rows (the Broadcast and Session tabs), kept
// following the newest. While the last row shows, a new message or a
// resize scrolls it back into view; scrolled up to read, the list stays
// put. A resize matters as well as a new message: a narrower window, or a
// smaller screen from a remote desktop, wraps long lines onto more rows,
// and wx keeps the top row where it was, so the newest lines went below
// the bottom and the list stopped following (Jeff, 2026-10-26).
class ChatList : public wxHtmlListBox
{
public:
    explicit ChatList(wxWindow* parent);

    // Sets the count and builds every row anew, then shows the newest row
    // if the list was following it.
    void showRows(size_t count);

private:
    bool lastRowShows() const;

    bool following_ = true;
};

#endif
