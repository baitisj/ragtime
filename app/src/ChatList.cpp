#include "ChatList.h"

ChatList::ChatList(wxWindow* parent)
    : wxHtmlListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME)
{
    SetMargins(FromDIP(4), FromDIP(1));
    // Every scroll is drawn, so the last paint says whether the operator
    // was reading the newest line: noted here, before any resize moves it.
    Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
        following_ = lastRowShows();
        e.Skip();
    });
    // The window has its new size, but the top row hasn't moved: scroll so
    // the newest row ends at the bottom again. wx's own handler runs after
    // this one, and a scroll bar coming or going sends another size.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        if (following_ && GetItemCount() > 0) ScrollToRow(GetItemCount() - 1);
        e.Skip();
    });
}

void ChatList::showRows(size_t count)
{
    bool follow = following_ || lastRowShows();
    SetItemCount(count);
    RefreshAll();
    if (follow && count > 0) ScrollToRow(count - 1);
    following_ = follow;
}

bool ChatList::lastRowShows() const
{
    return GetItemCount() == 0 || GetVisibleRowsEnd() >= GetItemCount();
}
