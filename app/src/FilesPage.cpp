#include "FilesPage.h"

#include <algorithm>
#include <utility>

#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/dc.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include "Html.h"

using namespace ragtime::files;

namespace
{

const wxColour MISSED(0xb4, 0x23, 0x18);    // red, as the Mismatch badge

enum
{
    ID_OPEN = wxID_HIGHEST + 700,
    ID_OPEN_FOLDER,
    ID_REMOVE,
};

// In progress and parts.
enum GoingColumn
{
    G_DATE,
    G_FILE,
    G_SIZE,
    G_PROGRESS,
    G_PEER,
    G_STATE,
    G_EXPIRY,
};

// Completed.
enum DoneColumn
{
    D_DATE,
    D_FILE,
    D_SIZE,
    D_PEER,
    D_RESULT,
    D_PATH,
};

wxString utf8(const std::string& text)
{
    return wxString::FromUTF8(text);
}

long long dayOf(uint64_t ms, long utcOffsetS)
{
    long long t = (long long)(ms / 1000) + utcOffsetS;
    return (t >= 0 ? t : t - 86399) / 86400;
}

std::string progressLabel(const FileRow& r)
{
    std::string text = progressText(r.done, r.size);
    if (r.look == FileRow::Look::Kept) return text + (r.outgoing ? " sent" : " kept");
    std::string rate = rateText(r.bytesPerSecond);
    if (!rate.empty()) text += " \xc2\xb7 " + rate;
    return text;
}

} // namespace

// The Progress column: the row's bar in two shades, acknowledged and only
// sent (or written and still arriving), or a broadcast file's pieces one
// by one, with "4,096 of 7,000 · 38 B/s" over it.
class ProgressRenderer : public wxDataViewCustomRenderer
{
public:
    explicit ProgressRenderer(const FilesPage* page) : wxDataViewCustomRenderer("long"), page_(page) {}

    bool SetValue(const wxVariant& value) override
    {
        index_ = value.GetLong();
        return true;
    }
    bool GetValue(wxVariant& value) const override
    {
        value = index_;
        return true;
    }
    wxSize GetSize() const override
    {
        wxWindow* view = GetView();
        int height = view ? view->GetCharHeight() + view->FromDIP(6) : 20;
        return wxSize(view ? view->FromDIP(170) : 170, height);
    }

    bool Render(wxRect cell, wxDC* dc, int state) override
    {
        const FileRow* r = index_ < 0 ? nullptr : page_->rowAt((size_t)index_);
        if (r == nullptr) return false;
        wxWindow* view = GetView();
        wxRect bar = cell.Deflate(view ? view->FromDIP(2) : 2, view ? view->FromDIP(3) : 3);
        wxColour surface = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
        wxColour track = blendColour(surface, secondaryText(), 0.18);
        wxColour handed = blendColour(surface, ACCENT_BLUE, 0.22);
        wxColour done = blendColour(surface, ACCENT_BLUE, 0.48);
        dc->SetPen(*wxTRANSPARENT_PEN);
        dc->SetBrush(wxBrush(track));
        dc->DrawRectangle(bar);
        auto span = [&](uint64_t from, uint64_t to, const wxColour& colour) {
            if (r->size == 0 || to <= from) return;
            int x0 = bar.x + (int)((double)bar.width * (double)from / (double)r->size);
            int x1 = bar.x + (int)((double)bar.width * (double)std::min(to, r->size) / (double)r->size);
            if (x1 <= x0) return;
            dc->SetBrush(wxBrush(colour));
            dc->DrawRectangle(x0, bar.y, x1 - x0, bar.height);
        };
        if (!r->pieces.empty())
        {
            // A broadcast file: each piece held, missed or still to come.
            size_t n = r->pieces.size();
            for (size_t i = 0; i < n; i++)
            {
                if (r->pieces[i] == PIECE_WAITING) continue;
                int x0 = bar.x + (int)((double)bar.width * (double)i / (double)n);
                int x1 = bar.x + (int)((double)bar.width * (double)(i + 1) / (double)n);
                dc->SetBrush(wxBrush(r->pieces[i] == PIECE_HELD ? done : blendColour(surface, MISSED, 0.35)));
                dc->DrawRectangle(x0, bar.y, std::max(1, x1 - x0), bar.height);
            }
        }
        else if (r->size == 0)
        {
            dc->SetBrush(wxBrush(r->look == FileRow::Look::Waiting ? track : done));
            dc->DrawRectangle(bar);
        }
        else
        {
            span(r->done, r->handed, handed);
            span(0, r->done, done);
        }
        RenderText(utf8(progressLabel(*r)), view ? view->FromDIP(6) : 6, cell, dc, state);
        return true;
    }

private:
    const FilesPage* page_;
    long index_ = -1;
};

FilesPage::FilesPage(wxWindow* parent, FileHistory& history) : wxPanel(parent), history_(history)
{
    goingTitle_ = new wxStaticText(this, wxID_ANY, "In progress and parts");
    goingTitle_->SetFont(goingTitle_->GetFont().Bold());
    going_ = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxDV_SINGLE | wxDV_ROW_LINES | wxDV_HORIZ_RULES);
    going_->AppendTextColumn("Date", wxDATAVIEW_CELL_INERT, FromDIP(84), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    going_->AppendTextColumn("File", wxDATAVIEW_CELL_INERT, FromDIP(140), wxALIGN_LEFT,
                             wxDATAVIEW_COL_RESIZABLE);
    going_->AppendTextColumn("Size", wxDATAVIEW_CELL_INERT, FromDIP(84), wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    auto* progress = new wxDataViewColumn("Progress", new ProgressRenderer(this), G_PROGRESS, FromDIP(170),
                                          wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    going_->AppendColumn(progress, "long");
    going_->AppendTextColumn("de", wxDATAVIEW_CELL_INERT, FromDIP(120), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    going_->AppendTextColumn("State", wxDATAVIEW_CELL_INERT, FromDIP(200), wxALIGN_LEFT,
                             wxDATAVIEW_COL_RESIZABLE);
    going_->AppendTextColumn("Expiry", wxDATAVIEW_CELL_INERT, FromDIP(120), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);

    doneTitle_ = new wxStaticText(this, wxID_ANY, "Completed");
    doneTitle_->SetFont(doneTitle_->GetFont().Bold());
    auto* openFolder = new wxButton(this, wxID_ANY, "Open received folder");
    openFolder->SetToolTip("Where files you save from a session go (Settings > Station)");
    done_ = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxDV_SINGLE | wxDV_ROW_LINES | wxDV_HORIZ_RULES);
    done_->AppendTextColumn("Date", wxDATAVIEW_CELL_INERT, FromDIP(84), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    done_->AppendTextColumn("File", wxDATAVIEW_CELL_INERT, FromDIP(140), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    done_->AppendTextColumn("Size", wxDATAVIEW_CELL_INERT, FromDIP(84), wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    // Bits a second over the whole transfer, to set beside the gear's speed.
    done_->AppendTextColumn("bps", wxDATAVIEW_CELL_INERT, FromDIP(64), wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    done_->AppendTextColumn("de", wxDATAVIEW_CELL_INERT, FromDIP(120), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    done_->AppendTextColumn("Result", wxDATAVIEW_CELL_INERT, FromDIP(220), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    done_->AppendTextColumn("Saved in", wxDATAVIEW_CELL_INERT, FromDIP(240), wxALIGN_LEFT,
                            wxDATAVIEW_COL_RESIZABLE);

    auto* doneHead = new wxBoxSizer(wxHORIZONTAL);
    doneHead->Add(doneTitle_, 1, wxALIGN_CENTER_VERTICAL);
    doneHead->Add(openFolder, 0, wxALIGN_CENTER_VERTICAL);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(goingTitle_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    sizer->Add(going_, 1, wxEXPAND | wxALL, 6);
    sizer->Add(doneHead, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    sizer->Add(done_, 1, wxEXPAND | wxALL, 6);
    SetSizer(sizer);

    openFolder->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onOpenFolder_) onOpenFolder_();
    });
    going_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent& e) { showRowMenu(e); });
    done_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent& e) { showFinishedMenu(e); });
    done_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& e) {
        if (const FinishedFile* f = finishedAt(done_->ItemToRow(e.GetItem()))) openFinished(*f, false);
    });
}

void FilesPage::setRows(std::vector<FileRow> rows, uint64_t nowMs, long utcOffsetS)
{
    // Newest first.
    std::stable_sort(rows.begin(), rows.end(), [](const FileRow& a, const FileRow& b) { return a.atMs > b.atMs; });
    auto key = [](const FileRow& r) { return std::make_pair((int)r.via, r.id); };

    bool same = rows.size() == rows_.size();
    for (size_t i = 0; same && i < rows.size(); i++) same = key(rows[i]) == key(rows_[i]);
    int picked = going_->GetSelectedRow();
    std::pair<int, uint64_t> pickedKey = picked != wxNOT_FOUND && (size_t)picked < rows_.size()
                                             ? key(rows_[(size_t)picked])
                                             : std::make_pair(-1, (uint64_t)0);
    rows_ = std::move(rows);

    auto values = [&](size_t i) {
        const FileRow& r = rows_[i];
        wxVector<wxVariant> v;
        v.push_back(utf8(dateText(r.atMs, nowMs, utcOffsetS)));
        v.push_back(utf8(r.name));
        v.push_back(utf8(sizeText(r.size)));
        v.push_back((long)i);
        v.push_back(utf8(peerText(r.outgoing, r.peer)));
        v.push_back(utf8(stateText(r, nowMs, utcOffsetS)));
        v.push_back(utf8(expiryText(r.expiresAtMs, nowMs, utcOffsetS)));
        return v;
    };
    if (same)
    {
        for (size_t i = 0; i < rows_.size(); i++)
        {
            wxVector<wxVariant> v = values(i);
            for (unsigned c = 0; c < v.size(); c++)
            {
                wxVariant old;
                going_->GetValue(old, (unsigned)i, c);
                if (c == G_PROGRESS || old != v[c]) going_->SetValue(v[c], (unsigned)i, c);
            }
        }
    }
    else
    {
        going_->DeleteAllItems();
        for (size_t i = 0; i < rows_.size(); i++)
        {
            going_->AppendItem(values(i));
            if (key(rows_[i]) == pickedKey) going_->SelectRow((unsigned)i);
        }
    }
    goingTitle_->SetLabel(rows_.empty() ? wxString("In progress and parts: nothing moving")
                                        : wxString::Format("In progress and parts (%zu)", rows_.size()));
    refreshHistory(nowMs, utcOffsetS);
}

void FilesPage::refreshHistory(uint64_t nowMs, long utcOffsetS)
{
    nowMs_ = nowMs;
    utcOffsetS_ = utcOffsetS;
    long long day = dayOf(nowMs, utcOffsetS);
    if (history_.version() == shownHistory_ && day == shownDay_) return;
    shownHistory_ = history_.version();
    shownDay_ = day;

    int picked = done_->GetSelectedRow();
    uint64_t pickedId = picked != wxNOT_FOUND && (size_t)picked < finishedIds_.size() ? finishedIds_[(size_t)picked] : 0;
    done_->DeleteAllItems();
    finishedIds_.clear();
    const auto& entries = history_.entries();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        const FinishedFile& f = *it;
        wxVector<wxVariant> v;
        v.push_back(utf8(dateText(f.atMs, nowMs, utcOffsetS)));
        v.push_back(utf8(f.name));
        v.push_back(utf8(sizeText(f.size)));
        v.push_back(utf8(bpsText(f)));
        v.push_back(utf8(peerText(f.outgoing, f.peer)));
        v.push_back(utf8(resultLabel(f)));
        bool saved = f.result == FinishedFile::Result::Saved;
        v.push_back(saved ? utf8(f.path) : wxString());
        done_->AppendItem(v);
        finishedIds_.push_back(f.id);
        if (f.id == pickedId) done_->SelectRow((unsigned)(finishedIds_.size() - 1));
    }
    doneTitle_->SetLabel(entries.empty() ? wxString("Completed: none yet")
                                         : wxString::Format("Completed (%zu)", entries.size()));
    Layout();
}

const FinishedFile* FilesPage::finishedAt(int row) const
{
    if (row == wxNOT_FOUND || (size_t)row >= finishedIds_.size()) return nullptr;
    return history_.find(finishedIds_[(size_t)row]);
}

void FilesPage::showRowMenu(wxDataViewEvent& e)
{
    int row = e.GetItem().IsOk() ? going_->ItemToRow(e.GetItem()) : going_->GetSelectedRow();
    if (row == wxNOT_FOUND || (size_t)row >= rows_.size()) return;
    going_->SelectRow((unsigned)row);
    // A copy: the rows can change while the menu is up.
    FileRow r = rows_[(size_t)row];
    wxMenu menu;
    if (onRowMenu_) onRowMenu_(r, menu);
    if (menu.GetMenuItemCount() > 0) PopupMenu(&menu);
}

void FilesPage::showFinishedMenu(wxDataViewEvent& e)
{
    int row = e.GetItem().IsOk() ? done_->ItemToRow(e.GetItem()) : done_->GetSelectedRow();
    const FinishedFile* found = finishedAt(row);
    if (found == nullptr) return;
    done_->SelectRow((unsigned)row);
    FinishedFile f = *found;
    wxMenu menu;
    if (onFinishedMenu_) onFinishedMenu_(f, menu);
    if (menu.GetMenuItemCount() > 0) menu.AppendSeparator();
    bool there = !f.path.empty() && wxFileName::FileExists(utf8(f.path));
    menu.Append(ID_OPEN, "&Open");
    menu.Enable(ID_OPEN, there);
    menu.Append(ID_OPEN_FOLDER, "Open &folder");
    menu.Enable(ID_OPEN_FOLDER, there);
    menu.AppendSeparator();
    menu.Append(ID_REMOVE, "&Remove from list");
    menu.Bind(wxEVT_MENU, [this, f](wxCommandEvent&) { openFinished(f, false); }, ID_OPEN);
    menu.Bind(wxEVT_MENU, [this, f](wxCommandEvent&) { openFinished(f, true); }, ID_OPEN_FOLDER);
    menu.Bind(wxEVT_MENU, [this, f](wxCommandEvent&) {
        history_.remove(f.id);
        refreshHistory(nowMs_, utcOffsetS_);
    }, ID_REMOVE);
    PopupMenu(&menu);
}

void FilesPage::openFinished(const FinishedFile& f, bool folder)
{
    if (f.path.empty()) return;
    if (folder)
        openWithDesktop(std::string(wxFileName(utf8(f.path)).GetPath().utf8_str()));
    else if (wxFileName::FileExists(utf8(f.path)))
        openWithDesktop(f.path);
}

void openWithDesktop(const std::string& utf8Path)
{
    if (utf8Path.empty()) return;
    wxLaunchDefaultApplication(utf8(utf8Path));
}
