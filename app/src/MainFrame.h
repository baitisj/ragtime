#ifndef RAGTIME_MAIN_FRAME_H
#define RAGTIME_MAIN_FRAME_H

#include <wx/frame.h>

class wxNotebook;

// The one main window. Package E fills it in (docs/STAGE1.md): connection
// bar, station list, the three tabs, Globus and the status bar.
class MainFrame : public wxFrame
{
public:
    MainFrame();

private:
    wxNotebook* tabs_ = nullptr;
};

#endif
