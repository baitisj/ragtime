#ifndef RAGTIME_RECENT_WORK_PANE_H
#define RAGTIME_RECENT_WORK_PANE_H

#include <cstdint>
#include <functional>
#include <string>

#include <wx/panel.h>

#include "ragtime/radio/RecentWork.h"

class wxChoice;
class wxSimplebook;
class RecentWorkList;

// Recent work, under the station list: the stations heard and worked in
// the path log's last two weeks, by default the ones likely around now
// first (radio::RecentWork). Each row is the call, on now or when last
// heard, and under it what puts it there: "At this hour 5 days, 17 m ·
// worked 3". Picking, double-clicking and right-clicking a row do what
// they do in the station list.
class RecentWorkPane : public wxPanel
{
public:
    using CallFn = std::function<void(const std::string& call)>;
    using BellFn = std::function<bool(const std::string& call)>;
    using OrderFn = std::function<void(ragtime::radio::WorkOrder order)>;

    explicit RecentWorkPane(wxWindow* parent);

    void onSelect(CallFn fn) { onSelect_ = std::move(fn); }
    void onActivate(CallFn fn) { onActivate_ = std::move(fn); }
    void onMenu(CallFn fn) { onMenu_ = std::move(fn); }
    // The order was picked from the pane's own menu.
    void onOrder(OrderFn fn) { onOrder_ = std::move(fn); }
    void bells(BellFn belled) { belled_ = std::move(belled); }

    void setOrder(ragtime::radio::WorkOrder order);
    ragtime::radio::WorkOrder order() const { return order_; }

    // New rows, keeping the selection while its station is still listed.
    void setWork(ragtime::radio::RecentWork work, uint64_t nowMs);
    // Draws the rows again (a bell came or went).
    void redraw();

    std::string selected() const { return selected_; }
    // Picking in the station list unpicks here, without a callback.
    void clearSelection();

private:
    friend class RecentWorkList;

    const ragtime::radio::WorkRow* rowAt(size_t row) const;
    void picked(int row);
    wxString tooltip(int row) const;
    void showSelection();

    wxChoice* orderChoice_ = nullptr;
    wxSimplebook* book_ = nullptr;
    RecentWorkList* list_ = nullptr;

    ragtime::radio::WorkOrder order_ = ragtime::radio::WorkOrder::LikelyNow;
    ragtime::radio::RecentWork work_;
    uint64_t nowMs_ = 0;
    std::string selected_;
    bool updating_ = false;

    CallFn onSelect_, onActivate_, onMenu_;
    OrderFn onOrder_;
    BellFn belled_;
};

#endif
