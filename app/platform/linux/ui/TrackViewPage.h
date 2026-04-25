// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/TagPopover.h"
#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackPresentation.h"

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace app::ui
{

  class TrackViewPage final : public Gtk::Box
  {
  public:
    using TrackId = TrackListAdapter::TrackId;
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId>, double, double)>;

    explicit TrackViewPage(rs::core::ListId listId,
                           Glib::RefPtr<TrackListAdapter> const& adapter,
                           std::shared_ptr<TrackColumnLayoutModel> columnLayoutModel);
    ~TrackViewPage() override;

    rs::core::ListId getListId() const { return _listId; }

    // Get the selected track IDs
    std::vector<TrackId> getSelectedTrackIds() const;

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

    // Show a context menu anchored to the current row coordinates.
    void showTagPopover(TagPopover& popover, double x, double y);

    // Status banner API
    void setStatusMessage(std::string const& message);
    void clearStatusMessage();

    // Navigation and Selection
    void selectTrack(TrackId trackId);

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
    void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
    void onActivateCurrentSelection();
    std::size_t selectedTrackCount() const;
    std::optional<TrackId> trackIdAtPosition(std::uint32_t position) const;

    struct ColumnBinding final
    {
      TrackColumn id;
      Glib::RefPtr<Gtk::ColumnViewColumn> column;
      Gtk::CheckButton* toggle = nullptr;
      int defaultWidth = -1;
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
    TagPopover* _tagPopover = nullptr;

    // Models
    rs::core::ListId _listId;
    Glib::RefPtr<TrackListAdapter> _adapter;
    Glib::RefPtr<Gtk::SortListModel> _sortModel;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
    std::shared_ptr<TrackColumnLayoutModel> _columnLayoutModel;
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

    // Signals
    SelectionChangedSignal _selectionChanged;
    TrackActivatedSignal _trackActivated;
    ContextMenuRequestedSignal _contextMenuRequested;
    TagEditRequestedSignal _tagEditRequested;
  };

} // namespace app::ui
