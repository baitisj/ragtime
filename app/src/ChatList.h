#ifndef RAGTIME_CHAT_LIST_H
#define RAGTIME_CHAT_LIST_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <wx/htmllbox.h>

// A chat's messages as HTML rows (the Broadcast and Session tabs), kept
// following the newest. While the last row shows, a new message or a
// resize scrolls it back into view; scrolled up to read, the list stays
// put. A resize matters as well as a new message: a narrower window, or a
// smaller screen from a remote desktop, wraps long lines onto more rows,
// and wx keeps the top row where it was, so the newest lines went below
// the bottom and the list stopped following (Jeff, 2026-10-26).
//
// Double-clicking a callsign in a row opens a ragchew with that station
// (Jeff, 2026-11-13): wx tells which word the first click of the two hit,
// and a word that is a callsign the row names, or a station Ragtime
// knows, goes to onCallsign.
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

    // A callsign double-clicked in a row: one the row names (callsIn), or
    // one isStation says is a station heard. Called after the click.
    void onCallsign(std::function<void(const std::string& call)> fn,
                    std::function<bool(const std::string& call)> isStation)
    {
        onCallsign_ = std::move(fn);
        isStation_ = std::move(isStation);
    }

protected:
    // The stations a row names: who sent it, who it is to, the Hands that
    // answered a roll call.
    virtual std::vector<std::string> callsIn(size_t row) const
    {
        (void)row;
        return {};
    }

private:
    // The double click's callsign, or "" when it didn't land on one.
    std::string callsignAt(const wxPoint& where) const;

    bool lastRowShows() const;
    // Scrolls so the newest row shows, as the list's own move.
    void pin();

    bool following_ = true;
    bool pinning_ = false;     // inside our own scroll
    size_t seenTop_ = 0;       // the top row at the last scroll bar update
    wxString clickedWord_;     // the word under the last click, and where it was
    wxPoint clickedAt_;
    std::function<void(const std::string&)> onCallsign_;
    std::function<bool(const std::string&)> isStation_;
};

#endif
