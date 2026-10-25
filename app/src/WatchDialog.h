#ifndef RAGTIME_WATCH_DIALOG_H
#define RAGTIME_WATCH_DIALOG_H

#include <string>
#include <vector>

#include <wx/dialog.h>

class wxButton;
class wxListBox;
class wxTextCtrl;

// Station > Watch list: the callsigns with a bell, heard before or not.
// Each rings (a desktop notification) when the station is heard on D2G or
// sends QRV. Fits a 1366x768 screen.
class WatchDialog : public wxDialog
{
public:
    WatchDialog(wxWindow* parent, const std::vector<std::string>& calls);

    std::vector<std::string> calls() const;

private:
    void add();
    void showButtons();

    wxTextCtrl* entry_ = nullptr;
    wxButton* add_ = nullptr;
    wxListBox* list_ = nullptr;
    wxButton* remove_ = nullptr;
};

#endif
