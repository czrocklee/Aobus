// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TagPopover.h"
#include "TrackListAdapter.h"
#include "TrackPresentation.h"

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk
{
  class TrackRowDataProvider;
  class MetadataCoordinator;

  class TrackViewPage final : public Gtk::Box
  {
  public:
    using TrackId = TrackListAdapter::TrackId;
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;
    using CreateSmartListRequestedSignal = sigc::signal<void(std::string)>;

    explicit TrackViewPage(ao::ListId listId,
                           TrackListAdapter& adapter,
                           TrackColumnLayoutModel& columnLayoutModel,
                           MetadataCoordinator& metadataCoordinator);
    ~TrackViewPage() override;

    ao::ListId getListId() const { return _listId; }

    // Get the selected track IDs
    std::vector<TrackId> getSelectedTrackIds() const;
    std::vector<Glib::RefPtr<TrackRow>> getSelectedRows() const;

    // Get total duration of selected tracks
    std::chrono::milliseconds getSelectedTracksDuration() const;

    // Get the visible playback order for the current songs view.
    std::vector<TrackId> getVisibleTrackIds() const;

    // Get the primary (first) selected track ID
    std::optional<TrackId> getPrimarySelectedTrackId() const;

    // Get the column view for context menu setup
    Gtk::ColumnView& getColumnView() { return _columnView; }

    // Signal for selection changes
    SelectionChangedSignal& signalSelectionChanged();

    // Signal for track activation (double-click or Enter)
    TrackActivatedSignal& signalTrackActivated();

    // Signal for secondary-click interactions on the current selection.
    ContextMenuRequestedSignal& signalContextMenuRequested();

    // Signal for tag edit requests (Ctrl+T or double-click on tags)
    TagEditRequestedSignal& signalTagEditRequested();

    // Signal for creating a smart list from the current filter expression.
    CreateSmartListRequestedSignal& signalCreateSmartListRequested();

    // Show a context menu anchored to the current row coordinates.
    void showTagPopover(TagPopover& popover, double x, double y);

    // Status banner API
    void setStatusMessage(std::string const& message);
    void clearStatusMessage();

    // Navigation and Selection
    void selectTrack(TrackId trackId);
    void setPlayingTrackId(std::optional<TrackId> trackId);
    void setFilterExpression(std::string const& expression);

  private:
    // Setup methods
    void setupColumns();
    void setupPresentationControls();
    void setupColumnControls();
    void setupHeaderFactory();
    void setupStatusBar();
    void setupActivation();
    void applyPresentationSpec();
    void applyColumnLayout();
    void syncColumnToggleStates();
    void queueSharedColumnLayoutUpdate();
    bool flushSharedColumnLayoutUpdate();
    void updateSharedColumnLayout();
    TrackColumnLayout captureCurrentColumnLayout() const;
    void updateColumnVisibility();
    void onGroupByChanged();
    void onFilterChanged();
    void updateFilterUi();
    void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
    void onActivateCurrentSelection();
    std::size_t selectedTrackCount() const;
    std::optional<TrackId> trackIdAtPosition(std::uint32_t position) const;

    struct ColumnBinding final
    {
      TrackColumn id;
      Glib::RefPtr<Gtk::ColumnViewColumn> column;
      Gtk::CheckButton* toggle = nullptr;
      std::int32_t defaultWidth = -1;
    };

    ColumnBinding* findColumnBinding(TrackColumn column);
    ColumnBinding const* findColumnBinding(TrackColumn column) const;

    // Child widgets
    Gtk::Box _controlsBar{Gtk::Orientation::HORIZONTAL};
    Gtk::Entry _filterEntry;
    Gtk::Label _groupByLabel;
    Gtk::DropDown _groupByDropdown;
    Gtk::MenuButton _columnsButton;
    Gtk::Popover _columnsPopover;
    Gtk::Box _columnsPopoverBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _columnsPopoverTitle;
    Gtk::ListBox _columnToggleList;
    Gtk::Separator _columnsPopoverSeparator;
    Gtk::Button _resetColumnsButton;
    Gtk::Label _statusLabel;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::ColumnView _columnView;
    Gtk::Popover _contextPopover;

    // Models
    ao::ListId _listId;
    TrackListAdapter& _adapter;
    MetadataCoordinator& _metadataCoordinator;
    Glib::RefPtr<Gtk::SortListModel> _sortModel;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
    TrackColumnLayoutModel& _columnLayoutModel;
    Glib::RefPtr<Gio::ListModel> _columnModel;
    Glib::RefPtr<Gtk::StringList> _groupByOptions;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactory;
    std::vector<ColumnBinding> _columns;
    TrackPresentationSpec _presentationSpec;
    sigc::connection _columnLayoutChangedConnection;
    sigc::connection _columnModelChangedConnection;
    sigc::connection _queuedColumnLayoutUpdateConnection;
    bool _syncingColumnLayout = false;
    bool _capturingColumnLayout = false;
    bool _suppressNextTrackActivation = false;
    Glib::RefPtr<Gtk::CssProvider> _dynamicCssProvider;
    std::vector<sigc::connection> _columnNotifyConnections;

    void updateTitlePositionVariable();
    Glib::RefPtr<Gtk::SignalListItemFactory> createTextColumnFactory(TrackColumnDefinition const& definition);

    // Signals
    SelectionChangedSignal _selectionChanged;
    TrackActivatedSignal _trackActivated;
    ContextMenuRequestedSignal _contextMenuRequested;
    TagEditRequestedSignal _tagEditRequested;
    CreateSmartListRequestedSignal _createSmartListRequested;
  };
} // namespace ao::gtk
