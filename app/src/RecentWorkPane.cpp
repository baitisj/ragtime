#include "RecentWorkPane.h"

#include <utility>

#include <wx/choice.h>
#include <wx/htmllbox.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "DrawnIcon.h"
#include "Html.h"
#include "Theme.h"
#include "ragtime/broadcast/Stations.h"

using namespace ragtime::radio;

namespace
{

constexpr int BELL_DIP = 12;

// The choice's items, in WorkOrder's order.
constexpr WorkOrder ORDERS[] = {WorkOrder::LikelyNow, WorkOrder::MostWorked, WorkOrder::Latest};

enum Page
{
    PAGE_LIST,
    PAGE_EMPTY,
};

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

} // namespace

//-------------------------------------------------------------------------
// The rows: two lines a station, as in the narrow station list.
//-------------------------------------------------------------------------

class RecentWorkList : public wxHtmlListBox
{
public:
    RecentWorkList(wxWindow* parent, RecentWorkPane* owner)
        : wxHtmlListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
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
        const WorkRow* r = owner_->rowAt(n);
        if (r == nullptr) return wxString();
        bool picked = IsSelected(n);
        wxString grey = picked ? wxString() : " color=\"" + htmlColour(secondaryText()) + "\"";
        wxString green = picked ? wxString() : " color=\"" + htmlColour(goodText()) + "\"";
        wxString bell = owner_->belled_ && owner_->belled_(r->call)
                            ? "&nbsp;" + bellHtml(this, BELL_DIP, picked ? wxSystemSettings::GetColour(
                                                                               wxSYS_COLOUR_HIGHLIGHTTEXT)
                                                                         : accentText())
                            : wxString();
        // On now, or when it was last heard (or worked, if never heard).
        wxString when;
        if (r->onNow)
            when = "<font" + green + ">" + utf8("\xe2\x97\x8f on now") + "</font>";
        else if (r->lastHeardMs != 0)
            when = "<font" + grey + ">" + htmlEscape(ragtime::broadcast::heardAgo(r->lastHeardMs, owner_->nowMs_))
                   + "</font>";
        else
            when = "<font" + grey + ">worked " + htmlEscape(ragtime::broadcast::heardAgo(r->lastWorkedMs, owner_->nowMs_))
                   + "</font>";
        wxString html;
        html << "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"1\">"
             << "<tr><td nowrap><b>" << htmlEscape(r->call) << "</b>" << bell << "</td>"
             << "<td nowrap align=\"right\">" << when << "</td></tr>"
             << "<tr><td nowrap colspan=\"2\"><font" << grey << ">" << htmlEscape(workLine(*r))
             << "</font></td></tr></table>";
        return html;
    }

private:
    RecentWorkPane* owner_;
};

//-------------------------------------------------------------------------
// The pane
//-------------------------------------------------------------------------

RecentWorkPane::RecentWorkPane(wxWindow* parent)
    : wxPanel(parent)
{
    followThemeBackground(this, wxSYS_COLOUR_LISTBOX);

    auto* title = new wxStaticText(this, wxID_ANY, "Recent work");
    title->SetFont(title->GetFont().Bold());
    followThemeText(title, []() { return wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT); });
    wxString why = "Stations you work often, likely ones first: the friends to look for at this hour, "
                   "from the path log's last two weeks.";
    title->SetToolTip(why);

    orderChoice_ = new wxChoice(this, wxID_ANY);
    for (WorkOrder o : ORDERS) orderChoice_->Append(workOrderLabel(o));
    orderChoice_->SetSelection(0);
    orderChoice_->SetToolTip(utf8("Likely now: (1 + days worked) \xc3\x97 (1 + days heard at this hour, on this band).\n"
                                  "Most worked: the most days with a session or a message.\n"
                                  "Latest: heard or worked most lately."));

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    header->Add(orderChoice_, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(3));

    book_ = new wxSimplebook(this);
    list_ = new RecentWorkList(book_, this);
    followThemeBackground(list_, wxSYS_COLOUR_LISTBOX);
    scrollbarBeside(list_);
    auto* empty = new wxPanel(book_);
    followThemeBackground(empty, wxSYS_COLOUR_LISTBOX);
    auto* hint = new wxStaticText(empty, wxID_ANY,
                                  "Nobody yet. The stations you hear and work gather here, the ones likely "
                                  "around at this hour first.",
                                  wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    followThemeText(hint, secondaryText);
    hint->Wrap(FromDIP(200));
    auto* emptySizer = new wxBoxSizer(wxVERTICAL);
    emptySizer->Add(hint, 0, wxEXPAND | wxALL, 12);
    empty->SetSizer(emptySizer);
    book_->AddPage(list_, "list");
    book_->AddPage(empty, "empty");
    book_->ChangeSelection(PAGE_EMPTY);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(header, 0, wxEXPAND);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);

    orderChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) {
        int i = e.GetSelection();
        if (i < 0 || i >= (int)(sizeof(ORDERS) / sizeof(ORDERS[0]))) return;
        order_ = ORDERS[i];
        if (onOrder_) onOrder_(order_);
    });
    list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) { picked(e.GetSelection()); });
    // As in the station list: the row under the mouse is what counts.
    list_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
        int row = list_->VirtualHitTest(e.GetPosition().y);
        if (row == wxNOT_FOUND) return;
        list_->SetSelection(row);
        picked(row);
        if (onActivate_ && !selected_.empty()) onActivate_(selected_);
    });
    list_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if ((e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) && onActivate_ && !selected_.empty())
            onActivate_(selected_);
        else
            e.Skip();
    });
    list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxPoint at = list_->ScreenToClient(e.GetPosition());
        int row = e.GetPosition() == wxDefaultPosition ? list_->GetSelection() : list_->VirtualHitTest(at.y);
        if (row != wxNOT_FOUND)
        {
            list_->SetSelection(row);
            picked(row);
        }
        // The row's tooltip would sit over the menu.
        list_->UnsetToolTip();
        if (onMenu_ && !selected_.empty()) onMenu_(selected_);
    });
    list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        list_->SetToolTip(tooltip(list_->VirtualHitTest(e.GetPosition().y)));
        e.Skip();
    });
}

void RecentWorkPane::setOrder(WorkOrder order)
{
    order_ = order;
    for (size_t i = 0; i < sizeof(ORDERS) / sizeof(ORDERS[0]); i++)
    {
        if (ORDERS[i] == order) orderChoice_->SetSelection((int)i);
    }
}

void RecentWorkPane::setWork(RecentWork work, uint64_t nowMs)
{
    work_ = std::move(work);
    nowMs_ = nowMs;
    bool listed = false;
    for (const WorkRow& r : work_.rows)
    {
        if (r.call == selected_) listed = true;
    }
    if (!selected_.empty() && !listed)
    {
        selected_.clear();
        if (onSelect_) onSelect_(selected_);
    }
    updating_ = true;
    book_->ChangeSelection(work_.rows.empty() ? PAGE_EMPTY : PAGE_LIST);
    if (list_->GetItemCount() != work_.rows.size()) list_->SetItemCount(work_.rows.size());
    showSelection();
    list_->RefreshAll();
    updating_ = false;
}

void RecentWorkPane::redraw()
{
    list_->RefreshAll();
}

void RecentWorkPane::clearSelection()
{
    selected_.clear();
    updating_ = true;
    showSelection();
    updating_ = false;
}

const WorkRow* RecentWorkPane::rowAt(size_t row) const
{
    return row < work_.rows.size() ? &work_.rows[row] : nullptr;
}

void RecentWorkPane::picked(int row)
{
    if (updating_) return;
    std::string call;
    if (const WorkRow* r = row >= 0 ? rowAt((size_t)row) : nullptr) call = r->call;
    if (call == selected_) return;
    selected_ = call;
    if (onSelect_) onSelect_(selected_);
}

void RecentWorkPane::showSelection()
{
    int row = wxNOT_FOUND;
    for (size_t i = 0; i < work_.rows.size(); i++)
    {
        if (work_.rows[i].call == selected_) row = (int)i;
    }
    if (list_->GetSelection() != row) list_->SetSelection(row);
}

wxString RecentWorkPane::tooltip(int row) const
{
    const WorkRow* r = row >= 0 ? rowAt((size_t)row) : nullptr;
    if (r == nullptr) return wxString();
    return utf8(workTip(*r, work_, nowMs_));
}
