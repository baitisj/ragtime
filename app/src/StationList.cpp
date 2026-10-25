#include "StationList.h"

#include <algorithm>
#include <utility>

#include <set>

#include <wx/filesys.h>
#include <wx/fs_mem.h>
#include <wx/htmllbox.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "DrawnIcon.h"
#include "Html.h"
#include "Theme.h"

using namespace ragtime::broadcast;

const int StationList::ID_COLUMN_FIRST = wxID_HIGHEST + 300;

namespace
{


wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

enum Page
{
    PAGE_NARROW,
    PAGE_TABLE,
    PAGE_EMPTY,
};

// A row lights this long when its QRV comes.
constexpr uint64_t FLASH_MS = 3000;
constexpr int BELL_DIP = 12;

// The bell for the narrow list's rows, drawn once a theme into wxHTML's
// memory files.
wxString bellHtml(const wxWindow* window, const wxColour& ink)
{
    int px = window->FromDIP(BELL_DIP);
    wxString name = wxString::Format("ragtime-bell-%s-%d.png", ink.GetAsString(wxC2S_HTML_SYNTAX).Mid(1), px);
    static std::set<wxString> made;
    if (made.count(name) == 0)
    {
        static bool handler = false;
        if (!handler)
        {
            // SealBadge may have added it already: wx keeps one of each.
            if (!wxFileSystem::HasHandlerForPath("memory:x")) wxFileSystem::AddHandler(new wxMemoryFSHandler);
            handler = true;
        }
        if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr) wxImage::AddHandler(new wxPNGHandler);
        wxBitmap bitmap = bellIcon(ink, BELL_DIP).GetBitmap(wxSize(px, px));
        if (!bitmap.IsOk()) return wxString();
        wxMemoryFSHandler::AddFile(name, bitmap.ConvertToImage(), wxBITMAP_TYPE_PNG);
        made.insert(name);
    }
    wxString side = wxString::Format("%d", px);
    return "<img src=\"memory:" + name + "\" width=\"" + side + "\" height=\"" + side + "\">";
}

} // namespace

//-------------------------------------------------------------------------
// The narrow list beside Globus: two lines a station.
//-------------------------------------------------------------------------

class NarrowStations : public wxHtmlListBox
{
public:
    explicit NarrowStations(StationList* owner)
        : wxHtmlListBox(owner->book_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , owner_(owner)
    {
        SetMargins(FromDIP(4), FromDIP(3));
    }

protected:
    wxString OnGetItemMarkup(size_t n) const override
    {
        return listText(OnGetItem(n), IsSelected(n));
    }

    wxString OnGetItem(size_t n) const override
    {
        const Station* s = owner_->stationAt(n);
        if (s == nullptr) return wxString();
        bool picked = IsSelected(n);
        StatusView status = owner_->stations_.status(*s, owner_->nowMs_);
        wxString grey = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
        // Green for available, orange for busy, blue for our own session.
        bool ours = s->call == owner_->stations_.partner();
        wxColour dot = !status.filled ? secondaryText() : ours ? accentText() : status.word == "QRV" ? goodText() : busyText();
        wxString tint = picked ? wxString() : " color=\"" + htmlColour(dot) + "\"";
        wxString bell = owner_->belled(s->call) ? "&nbsp;" + bellHtml(this, picked ? wxSystemSettings::GetColour(
                                                                       wxSYS_COLOUR_HIGHLIGHTTEXT) : accentText())
                                                : wxString();
        wxString lit = owner_->flashing(s->call) && !picked
                           ? " bgcolor=\"" + htmlColour(blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX),
                                                                    accentText(), 0.25)) + "\""
                           : wxString();
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"1\"" << lit << ">"
             << "<tr><td nowrap><b>" << htmlEscape(s->call) << "</b>" << bell << "&nbsp;&nbsp;<font" << tint << ">"
             << utf8(status.filled ? "\xe2\x97\x8f " : "\xe2\x97\x8b ") << htmlEscape(status.word) << "</font></td>"
             << "<td nowrap align=\"right\"><font" << grey << ">" << htmlEscape(heardAgo(s->lastHeardMs, owner_->nowMs_))
             << "</font></td></tr>"
             << "<tr><td nowrap><font" << grey << ">" << htmlEscape(owner_->stations_.whereLine(*s))
             << "</font></td>"
             // The last burst's SNR, under the time, from a host that reports one.
             << "<td nowrap align=\"right\"><font" << grey << ">"
             << htmlEscape(owner_->stations_.cell(*s, Column::Snr, owner_->nowMs_)) << "</font></td></tr></table>";
        return html;
    }

private:
    StationList* owner_;
};

//-------------------------------------------------------------------------
// The table, with Globus off.
//-------------------------------------------------------------------------

class StationTable : public wxListCtrl
{
public:
    explicit StationTable(StationList* owner)
        : wxListCtrl(owner->book_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxBORDER_NONE)
        , owner_(owner)
    {
    }

protected:
    wxString OnGetItemText(long item, long column) const override
    {
        const Station* s = owner_->stationAt((size_t)item);
        if (s == nullptr || column < 0 || column >= (long)owner_->columns_.size()) return wxString();
        return utf8(owner_->stations_.cell(*s, owner_->columns_[(size_t)column], owner_->nowMs_));
    }

    // The bell beside the call, in the first column.
    int OnGetItemImage(long item) const override
    {
        const Station* s = owner_->stationAt((size_t)item);
        return s != nullptr && owner_->belled(s->call) ? 0 : -1;
    }

    int OnGetItemColumnImage(long item, long column) const override
    {
        return column == 0 ? OnGetItemImage(item) : -1;
    }

    wxItemAttr* OnGetItemAttr(long item) const override
    {
        const Station* s = owner_->stationAt((size_t)item);
        if (s == nullptr || !owner_->flashing(s->call)) return nullptr;
        lit_.SetBackgroundColour(blendColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX), accentText(), 0.25));
        return &lit_;
    }

private:
    StationList* owner_;
    mutable wxItemAttr lit_;
};

//-------------------------------------------------------------------------

StationList::StationList(wxWindow* parent, const Stations& stations)
    : wxPanel(parent)
    , stations_(stations)
{
    columns_ = defaultColumns();
    book_ = new wxSimplebook(this);
    narrowView_ = new NarrowStations(this);
    followThemeBackground(narrowView_, wxSYS_COLOUR_LISTBOX);
    scrollbarBeside(narrowView_);
    table_ = new StationTable(this);
    followThemeBackground(table_, wxSYS_COLOUR_LISTBOX);
    {
        int px = FromDIP(BELL_DIP);
        auto* images = new wxImageList(px, px, true, 1);
        images->Add(bellIcon(accentText(), BELL_DIP).GetBitmap(wxSize(px, px)));
        table_->AssignImageList(images, wxIMAGE_LIST_SMALL);
    }

    auto* empty = new wxPanel(book_);
    followThemeBackground(empty, wxSYS_COLOUR_LISTBOX);
    auto* hint = new wxStaticText(empty, wxID_ANY,
                                  "No stations heard yet. They appear here as their broadcasts are heard.",
                                  wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    followThemeText(hint, secondaryText);
    hint->Wrap(FromDIP(180));
    auto* emptySizer = new wxBoxSizer(wxVERTICAL);
    emptySizer->Add(hint, 0, wxEXPAND | wxALL, 16);
    empty->SetSizer(emptySizer);

    book_->AddPage(narrowView_, "narrow");
    book_->AddPage(table_, "table");
    book_->AddPage(empty, "empty");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);
    rebuildColumns();

    narrowView_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) { picked(e.GetSelection()); });
    // wxVListBox counts a double click only on the row already current,
    // which a refresh can move; the row under the mouse is what counts.
    narrowView_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
        int row = narrowView_->VirtualHitTest(e.GetPosition().y);
        if (row == wxNOT_FOUND) return;
        narrowView_->SetSelection(row);
        picked(row);
        if (onActivate_ && !selected_.empty()) onActivate_(selected_);
    });
    narrowView_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if ((e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) && onActivate_ && !selected_.empty())
            onActivate_(selected_);
        else
            e.Skip();
    });
    narrowView_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxPoint at = narrowView_->ScreenToClient(e.GetPosition());
        int row = e.GetPosition() == wxDefaultPosition ? narrowView_->GetSelection() : narrowView_->VirtualHitTest(at.y);
        if (row != wxNOT_FOUND)
        {
            narrowView_->SetSelection(row);
            picked(row);
        }
        if (onMenu_ && !selected_.empty()) onMenu_(selected_);
    });
    narrowView_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        narrowView_->SetToolTip(tooltip(narrowView_->VirtualHitTest(e.GetPosition().y)));
        e.Skip();
    });

    table_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { picked(e.GetIndex()); });
    table_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) {
        if (!updating_) picked(-1);
    });
    table_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
        picked(e.GetIndex());
        if (onActivate_ && !selected_.empty()) onActivate_(selected_);
    });
    table_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& e) {
        table_->SetItemState(e.GetIndex(), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                             wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        picked(e.GetIndex());
        if (onMenu_ && !selected_.empty()) onMenu_(selected_);
    });
    table_->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
        int col = e.GetColumn();
        if (col < 0 || col >= (int)columns_.size()) return;
        Column c = columns_[(size_t)col];
        setSort(c, c == sortColumn_ ? !sortAscending_ : true);
    });
    table_->Bind(wxEVT_LIST_COL_RIGHT_CLICK, [this](wxListEvent&) {
        wxMenu menu;
        appendColumnItems(menu);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) { toggleColumn(e.GetId()); });
        PopupMenu(&menu);
    });
    table_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        int flags = 0;
        long row = table_->HitTest(e.GetPosition(), flags);
        table_->SetToolTip(tooltip(row));
        e.Skip();
    });

    setNarrow(true);
}

const Station* StationList::stationAt(size_t row) const
{
    if (row >= order_.size()) return nullptr;
    return stations_.find(order_[row]);
}

wxString StationList::tooltip(long row) const
{
    if (row < 0) return wxString();
    const Station* s = stationAt((size_t)row);
    if (s == nullptr) return wxString();
    std::string tip = stations_.status(*s, nowMs_).how;
    if (!s->software.empty()) tip += "\n" + s->software;
    if (belled(s->call) && bellTip_) tip += "\n" + bellTip_(s->call);
    return utf8(tip);
}

void StationList::bells(BellFn belled, BellTipFn tip)
{
    belled_ = std::move(belled);
    bellTip_ = std::move(tip);
    refresh(nowMs_);
}

void StationList::flash(const std::string& call, uint64_t nowMs)
{
    flashCall_ = call;
    flashUntilMs_ = nowMs + FLASH_MS;
    refresh(nowMs);
}

bool StationList::flashing(const std::string& call) const
{
    return !flashCall_.empty() && call == flashCall_ && nowMs_ < flashUntilMs_;
}

void StationList::picked(long row)
{
    if (updating_) return;
    std::string call;
    if (row >= 0 && (size_t)row < order_.size()) call = order_[(size_t)row];
    if (call == selected_) return;
    selected_ = call;
    if (onSelect_) onSelect_(selected_);
}

void StationList::setNarrow(bool narrow)
{
    narrow_ = narrow;
    refresh(nowMs_);
}

void StationList::setColumns(const std::vector<Column>& columns)
{
    columns_.clear();
    for (Column c : columns)
    {
        if (std::find(columns_.begin(), columns_.end(), c) == columns_.end())
            columns_.push_back(c);
    }
    if (std::find(columns_.begin(), columns_.end(), Column::Call) == columns_.end())
        columns_.insert(columns_.begin(), Column::Call);
    rebuildColumns();
    refresh(nowMs_);
}

void StationList::appendColumnItems(wxMenu& menu) const
{
    for (Column c : ALL_COLUMNS)
    {
        wxMenuItem* item = menu.AppendCheckItem(ID_COLUMN_FIRST + (int)c, columnLabel(c));
        item->Check(std::find(columns_.begin(), columns_.end(), c) != columns_.end());
        // The call names the row, so it always shows.
        item->Enable(c != Column::Call);
    }
}

void StationList::toggleColumn(int menuId)
{
    int index = menuId - ID_COLUMN_FIRST;
    if (index < 0 || index >= COLUMN_COUNT) return;
    Column c = static_cast<Column>(index);
    if (c == Column::Call) return;
    std::vector<Column> next;
    bool shown = std::find(columns_.begin(), columns_.end(), c) != columns_.end();
    // Shown columns keep the table's order.
    for (Column each : ALL_COLUMNS)
    {
        bool on = each == c ? !shown : std::find(columns_.begin(), columns_.end(), each) != columns_.end();
        if (on) next.push_back(each);
    }
    setColumns(next);
}

void StationList::rebuildColumns()
{
    table_->ClearAll();
    for (size_t i = 0; i < columns_.size(); i++)
    {
        Column c = columns_[i];
        int width = c == Column::Status ? 125 : c == Column::Heard ? 90 : c == Column::Distance ? 85 : c == Column::Call ? 80
                    : c == Column::Bearing ? 65 : c == Column::Key ? 40 : 75;
        int align = c == Column::Distance || c == Column::Bearing || c == Column::Snr ? wxLIST_FORMAT_RIGHT
                                                                                      : wxLIST_FORMAT_LEFT;
        table_->AppendColumn(columnLabel(c), (wxListColumnFormat)align, FromDIP(width));
    }
    for (size_t i = 0; i < columns_.size(); i++)
    {
        if (columns_[i] == sortColumn_) table_->ShowSortIndicator((int)i, sortAscending_);
    }
}

void StationList::setSort(Column column, bool ascending)
{
    sortColumn_ = column;
    sortAscending_ = ascending;
    table_->RemoveSortIndicator();
    for (size_t i = 0; i < columns_.size(); i++)
    {
        if (columns_[i] == sortColumn_) table_->ShowSortIndicator((int)i, sortAscending_);
    }
    refresh(nowMs_);
}

void StationList::refresh(uint64_t nowMs)
{
    nowMs_ = nowMs;
    order_.clear();
    for (const Station* s : stations_.sorted(sortColumn_, sortAscending_, nowMs)) order_.push_back(s->call);
    if (!selected_.empty() && std::find(order_.begin(), order_.end(), selected_) == order_.end())
    {
        selected_.clear();
        if (onSelect_) onSelect_(selected_);
    }

    updating_ = true;
    int page = narrow_ ? (order_.empty() ? PAGE_EMPTY : PAGE_NARROW) : PAGE_TABLE;
    if (book_->GetSelection() != page) book_->ChangeSelection((size_t)page);
    if (narrowView_->GetItemCount() != order_.size()) narrowView_->SetItemCount(order_.size());
    // A new count leaves wx's table to lay itself out again, and a paint
    // before that draws it empty: it blinked once a refresh (Jeff,
    // 2026-10-23). So the count is set only when it changes, and then the
    // table repaints itself once laid out.
    bool recount = table_->GetItemCount() != (long)order_.size();
    if (recount) table_->SetItemCount((long)order_.size());
    showSelection();
    narrowView_->RefreshAll();
    if (!recount) table_->Refresh();
    updating_ = false;
}

void StationList::showSelection()
{
    int row = wxNOT_FOUND;
    for (size_t i = 0; i < order_.size(); i++)
    {
        if (order_[i] == selected_) row = (int)i;
    }
    if (narrowView_->GetSelection() != row) narrowView_->SetSelection(row);
    long current = table_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (current != row)
    {
        if (current >= 0) table_->SetItemState(current, 0, wxLIST_STATE_SELECTED);
        if (row >= 0) table_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
}
