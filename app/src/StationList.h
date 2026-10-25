#ifndef RAGTIME_STATION_LIST_H
#define RAGTIME_STATION_LIST_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "ragtime/broadcast/Stations.h"

class wxListCtrl;
class wxMenu;
class wxSimplebook;
class NarrowStations;
class StationTable;

// The station list on the left (docs/MOCKUPS.md, "Station list"). Beside
// Globus it is narrow: each row the callsign and status with the last
// heard time, and under it the locator and distance. With Globus off it is
// a table whose columns come from the Columns menu, sorted by any column.
// Either way it is sorted the same, and hovering a row says how its status
// is known.
class StationList : public wxPanel
{
public:
    // Menu ids for the Columns menu: ID_COLUMN_FIRST + the column.
    static const int ID_COLUMN_FIRST;

    using CallFn = std::function<void(const std::string& call)>;

    StationList(wxWindow* parent, const ragtime::broadcast::Stations& stations);

    // Picked (or none: ""), double-clicked or Enter, right-clicked.
    void onSelect(CallFn fn) { onSelect_ = std::move(fn); }
    void onActivate(CallFn fn) { onActivate_ = std::move(fn); }
    void onMenu(CallFn fn) { onMenu_ = std::move(fn); }

    void setNarrow(bool narrow);
    bool narrow() const { return narrow_; }

    // Redraws from the station list, keeping the selection. Every second
    // for the Heard times, and on every change.
    void refresh(uint64_t nowMs);

    std::string selected() const { return selected_; }
    // Picking in Recent work unpicks here, without a callback.
    void clearSelection();

    // A bell on a row (the watch list, Tell me when free), with how it
    // came to be for the row's tooltip.
    using BellFn = std::function<bool(const std::string& call)>;
    using BellTipFn = std::function<std::string(const std::string& call)>;
    void bells(BellFn belled, BellTipFn tip);
    // Lights a row for a moment: its QRV has come (docs/PLAN.md, "The
    // station list shows it").
    void flash(const std::string& call, uint64_t nowMs);

    // The table's columns, and the Columns menu that picks them.
    const std::vector<ragtime::broadcast::Column>& columns() const { return columns_; }
    void setColumns(const std::vector<ragtime::broadcast::Column>& columns);
    void appendColumnItems(wxMenu& menu) const;
    // A Columns menu id was picked: shows or hides that column.
    void toggleColumn(int menuId);

    void setSort(ragtime::broadcast::Column column, bool ascending);
    ragtime::broadcast::Column sortColumn() const { return sortColumn_; }
    bool sortAscending() const { return sortAscending_; }

private:
    friend class NarrowStations;
    friend class StationTable;

    const ragtime::broadcast::Station* stationAt(size_t row) const;
    void picked(long row);
    void rebuildColumns();
    void showSelection();
    wxString tooltip(long row) const;
    bool belled(const std::string& call) const { return belled_ && belled_(call); }
    bool flashing(const std::string& call) const;

    const ragtime::broadcast::Stations& stations_;
    wxSimplebook* book_ = nullptr;
    NarrowStations* narrowView_ = nullptr;
    StationTable* table_ = nullptr;
    bool narrow_ = true;

    std::vector<ragtime::broadcast::Column> columns_;
    ragtime::broadcast::Column sortColumn_ = ragtime::broadcast::Column::Heard;
    bool sortAscending_ = true;

    std::vector<std::string> order_;        // the calls, as shown
    std::string selected_;
    uint64_t nowMs_ = 0;
    bool updating_ = false;

    CallFn onSelect_, onActivate_, onMenu_;
    BellFn belled_;
    BellTipFn bellTip_;
    std::string flashCall_;
    uint64_t flashUntilMs_ = 0;
};

#endif
