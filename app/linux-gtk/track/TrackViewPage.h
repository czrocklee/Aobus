// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "tag/TagPopover.h"
#include "track/TrackColumnController.h"
#include "track/TrackFilterController.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackSelectionController.h"

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackRowCache;

  class TrackViewPage final : public Gtk::Box
  {
  public:
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;
    using CreateSmartListRequestedSignal = sigc::signal<void(std::string)>;

    explicit TrackViewPage(ListId listId,
                           TrackListAdapter& adapter,
                           TrackColumnLayoutModel& columnLayoutModel,
                           rt::AppSession& session,
                           rt::ViewId viewId = rt::ViewId{});
    ~TrackViewPage() override;

    ListId getListId() const { return _listId; }

    Gtk::ColumnView& getColumnView() { return _columnView; }

    TrackFilterController& filterController() { return *_filterController; }
    TrackSelectionController& selectionController() { return *_selectionController; }

    CreateSmartListRequestedSignal& signalCreateSmartListRequested();
    rt::ITrackListProjection* projection() const { return _adapter.projection(); }

    void showTagPopover(TagPopover& popover, double x, double y);
    void setStatusMessage(std::string_view message);
    void clearStatusMessage();

  private:
    void setupPresentationControls();
    void setupHeaderFactory();
    void setupStatusBar();
    void updateSectionHeaders();
    void onGroupByChanged();
    void onModelChanged();

    void commitMetadataChange(Glib::RefPtr<TrackRowObject> const& row, TrackColumn column, std::string const& newValue);

    // Child widgets
    Gtk::Box _controlsBar{Gtk::Orientation::HORIZONTAL};
    Gtk::Entry _filterEntry;
    Gtk::Label _groupByLabel;
    Gtk::DropDown _groupByDropdown;
    Gtk::Label _statusLabel;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::ColumnView _columnView;
    Gtk::Popover _contextPopover;

    // Models
    ListId _listId;
    rt::ViewId _viewId{};
    TrackListAdapter& _adapter;
    rt::AppSession& _session;
    Glib::RefPtr<Gtk::SortListModel> _groupModel;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
    TrackColumnLayoutModel& _columnLayoutModel;
    Glib::RefPtr<Gtk::StringList> _groupByOptions;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactory;
    rt::TrackGroupKey _activeGroupBy = rt::TrackGroupKey::None;

    // Controllers (owned)
    std::unique_ptr<TrackColumnController> _columnController;
    std::unique_ptr<TrackFilterController> _filterController;
    std::unique_ptr<TrackSelectionController> _selectionController;

    // Signals
    CreateSmartListRequestedSignal _createSmartListRequested;
  };
} // namespace ao::gtk
