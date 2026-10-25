#ifndef RAGTIME_PATH_STRIP_VIEW_H
#define RAGTIME_PATH_STRIP_VIEW_H

#include <cstdint>

#include <wx/window.h>

#include "ragtime/radio/PathLog.h"

// A station's 24-hour strip by band, from the path log: a row for each
// band it was heard on, a cell for each UTC hour, darker for the hours it
// was heard on more of the days, and the hour now marked. Hovering a cell
// gives its bursts, days and median S/N. Small in the station details
// under Globus; bigger in the paths dialog.
class PathStripView : public wxWindow
{
public:
    // cellDip: each hour's cell is about this wide, and its row as tall.
    PathStripView(wxWindow* parent, int cellDip);

    void setStrip(const ragtime::radio::PathStrip& strip, uint64_t nowMs);
    const ragtime::radio::PathStrip& strip() const { return strip_; }

    wxSize DoGetBestClientSize() const override;

private:
    void onPaint();
    void onMotion(const wxPoint& at);
    int labelWidth() const;
    int cellWidth() const;
    int rowHeight() const;

    ragtime::radio::PathStrip strip_;
    uint64_t nowMs_ = 0;
    int cellDip_;
};

#endif
