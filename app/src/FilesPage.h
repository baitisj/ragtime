#ifndef RAGTIME_FILES_PAGE_H
#define RAGTIME_FILES_PAGE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "ragtime/files/FileRows.h"

class wxDataViewEvent;
class wxDataViewListCtrl;
class wxMenu;
class wxStaticText;

// The Files tab (docs/PLAN.md, "The Files tab"): two panes, newest first.
// "In progress and parts" holds every transfer not finished, session files
// and broadcast files alike, with a two-shade bar (acknowledged, and sent
// but not yet acknowledged; a broadcast file's pieces each in turn), the
// rate over the last 2 minutes and one finish estimate. "Completed" is the
// lasting log, FileHistory.
//
// The page draws; the window owns the engines. It hands the page every
// row in progress with setRows(), and fills each row's right-click menu
// (onRowMenu: by the row's via, session files or broadcast files). The
// page itself offers Open, Open folder and Remove from list on a finished
// row, after anything onFinishedMenu adds.
class FilesPage : public wxPanel
{
public:
    FilesPage(wxWindow* parent, ragtime::files::FileHistory& history);

    // Every row in progress, any order; nowMs and utcOffsetS for the
    // clock times. Redrawn in place, keeping the selection.
    void setRows(std::vector<ragtime::files::FileRow> rows, uint64_t nowMs, long utcOffsetS);
    // Redraws Completed when the log has changed, or the day has.
    void refreshHistory(uint64_t nowMs, long utcOffsetS);

    // Right-click on a row in progress: fill the menu (and bind its items);
    // nothing is shown if it stays empty.
    void onRowMenu(std::function<void(const ragtime::files::FileRow&, wxMenu&)> fn) { onRowMenu_ = std::move(fn); }
    // Right-click on a finished one: items before Open and the rest.
    void onFinishedMenu(std::function<void(const ragtime::files::FinishedFile&, wxMenu&)> fn)
    {
        onFinishedMenu_ = std::move(fn);
    }
    void onOpenFolder(std::function<void()> fn) { onOpenFolder_ = std::move(fn); }
    // Voice messages (docs/STAGE1.md, "Voice cards"): a finished one shows
    // a play sign before its name (a stop sign while it plays), and a
    // double click or Play on its menu plays it, or stops it.
    void onPlayVoice(std::function<void(const ragtime::files::FinishedFile&)> play,
                     std::function<bool(const std::string& path)> playing)
    {
        onPlayVoice_ = std::move(play);
        isPlaying_ = std::move(playing);
    }
    // The play and stop signs again, when what plays has changed.
    void showPlaying();

    // Draws the bar in the Progress column.
    const ragtime::files::FileRow* rowAt(size_t n) const { return n < rows_.size() ? &rows_[n] : nullptr; }

private:
    void showRowMenu(wxDataViewEvent& e);
    void showFinishedMenu(wxDataViewEvent& e);
    void openFinished(const ragtime::files::FinishedFile& f, bool folder);
    // A finished file's name in the list: a voice message's with its sign.
    wxString finishedName(const ragtime::files::FinishedFile& f) const;
    const ragtime::files::FinishedFile* finishedAt(int row) const;

    ragtime::files::FileHistory& history_;
    wxDataViewListCtrl* going_ = nullptr;
    wxDataViewListCtrl* done_ = nullptr;
    wxStaticText* goingTitle_ = nullptr;
    wxStaticText* doneTitle_ = nullptr;
    std::vector<ragtime::files::FileRow> rows_;
    std::vector<uint64_t> finishedIds_;     // Completed's rows, top first
    uint64_t shownHistory_ = ~0ull;
    long long shownDay_ = 0;
    uint64_t nowMs_ = 0;
    long utcOffsetS_ = 0;

    std::function<void(const ragtime::files::FileRow&, wxMenu&)> onRowMenu_;
    std::function<void(const ragtime::files::FinishedFile&, wxMenu&)> onFinishedMenu_;
    std::function<void()> onOpenFolder_;
    std::function<void(const ragtime::files::FinishedFile&)> onPlayVoice_;
    std::function<bool(const std::string&)> isPlaying_;
};

// Opens a file or a folder with the desktop's own program.
void openWithDesktop(const std::string& utf8Path);

#endif
