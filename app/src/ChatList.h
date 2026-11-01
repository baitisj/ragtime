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
//
// Whether it follows is settled the moment the operator scrolls, not at
// the next paint. On GTK a scroll's own scroll bar update sends a size
// event at once, before anything is drawn; judged at paint, the list
// still thought it was following then and jumped back to the bottom, so
// the wheel and the scroll bar couldn't move it (Jeff, 2026-11-01).
class ChatList : public wxHtmlListBox
{
public:
    explicit ChatList(wxWindow* parent);

    // Sets the count and builds every row anew, then shows the newest row
    // if the list was following it.
    void showRows(size_t count);

    // True while the newest row is kept in view.
    bool following() const { return following_; }

    // Every scroll comes through here, with the new top row already set
    // and before the scroll bar is told: a scroll that wasn't ours is the
    // operator's, and says whether the newest row still shows.
    void UpdateScrollbar() override;

private:
    bool lastRowShows() const;
    // Scrolls so the newest row shows, as the list's own move.
    void pin();

    bool following_ = true;
    bool pinning_ = false;     // inside our own scroll
    size_t seenTop_ = 0;       // the top row at the last scroll bar update
};

#endif
