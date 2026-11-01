#include "ChatList.h"

ChatList::ChatList(wxWindow* parent)
    : wxHtmlListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME)
{
    SetMargins(FromDIP(4), FromDIP(1));
    // The window has its new size, but the top row hasn't moved: scroll so
    // the newest row ends at the bottom again. wx's own handler runs after
    // this one, and a scroll bar coming or going sends another size.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        if (following_) pin();
        e.Skip();
    });
}

void ChatList::showRows(size_t count)
{
    bool follow = following_ || lastRowShows();
    pinning_ = true;
    SetItemCount(count);
    RefreshAll();
    pinning_ = false;
    following_ = follow;
    seenTop_ = GetVisibleRowsBegin();
    if (follow) pin();
}

void ChatList::UpdateScrollbar()
{
    // Decided before wx tells the scroll bar, which on GTK can send a size
    // event straight away.
    const size_t top = GetVisibleRowsBegin();
    if (!pinning_ && top != seenTop_) following_ = lastRowShows();
    seenTop_ = top;
    wxHtmlListBox::UpdateScrollbar();
}

void ChatList::pin()
{
    if (GetItemCount() == 0) return;
    pinning_ = true;
    ScrollToRow(GetItemCount() - 1);
    pinning_ = false;
    seenTop_ = GetVisibleRowsBegin();
}

bool ChatList::lastRowShows() const
{
    // Worked out from the rows' heights rather than wx's count of rows on
    // screen, which is only brought up to date after this is asked.
    const size_t count = GetItemCount();
    return count == 0 || GetVisibleRowsBegin() >= FindFirstVisibleFromLast(count - 1);
}
