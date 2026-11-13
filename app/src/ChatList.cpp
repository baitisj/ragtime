#include "ChatList.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <wx/html/htmlcell.h>
#include <wx/html/htmlwin.h>

#include "ragtime/protocol/Fields.h"

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
    // wx says which cell a click hit, but not on a double click: the
    // word under the first click of the two is kept for it.
    Bind(wxEVT_HTML_CELL_CLICKED, [this](wxHtmlCellEvent& e) {
        const wxHtmlCell* cell = e.GetCell();
        clickedWord_ = cell != nullptr ? cell->ConvertToText(nullptr) : wxString();
        clickedAt_ = e.GetMouseEvent().GetPosition();
        e.Skip();
    });
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
        e.Skip();
        if (!onCallsign_) return;
        std::string call = callsignAt(e.GetPosition());
        clickedWord_.clear();
        if (call.empty()) return;
        auto fn = onCallsign_;
        CallAfter([fn, call]() { fn(call); });
    });
}

std::string ChatList::callsignAt(const wxPoint& where) const
{
    // The first click of the two, where the second landed.
    const int slack = FromDIP(4);
    if (clickedWord_.empty() || std::abs(where.x - clickedAt_.x) > slack || std::abs(where.y - clickedAt_.y) > slack)
        return std::string();
    int row = VirtualHitTest(where.y);
    if (row == wxNOT_FOUND) return std::string();
    std::string call = ragtime::protocol::callsignInWord(std::string(clickedWord_.utf8_str()));
    if (call.empty()) return std::string();
    std::vector<std::string> named = callsIn((size_t)row);
    if (std::find(named.begin(), named.end(), call) != named.end()) return call;
    return isStation_ && isStation_(call) ? call : std::string();
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
