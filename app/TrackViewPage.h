// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackListAdapter.h"
#include "TrackPresentation.h"

#include <gtkmm.h>

#include <cstdint>
#include <vector>

class TrackViewPage final : public Gtk::Box
{
public:
  using TrackId = TrackListAdapter::TrackId;
  using SelectionChangedSignal = sigc::signal<void()>;
  using TrackActivatedSignal = sigc::signal<void(TrackId)>;
  using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;

  explicit TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter);
  ~TrackViewPage() override;

  // Get the selected track IDs
  std::vector<TrackId> getSelectedTrackIds() const;

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

  // Show a context menu anchored to the current row coordinates.
  void popupContextMenu(Glib::RefPtr<Gio::MenuModel> const& model, double x, double y);

  // Status banner API
  void setStatusMessage(std::string const& message);
  void clearStatusMessage();

private:
  // Setup methods
  void setupColumns();
  void setupPresentationControls();
  void setupHeaderFactory();
  void setupStatusBar();
  void setupActivation();
  void applyPresentationSpec();
  void updateColumnVisibility();
  void onGroupByChanged();
  void onFilterChanged();
  void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
  void onActivateCurrentSelection();
  std::size_t selectedTrackCount() const;
  std::optional<TrackId> trackIdAtPosition(std::uint32_t position) const;

  // Child widgets
  Gtk::Box _controlsBar{Gtk::Orientation::HORIZONTAL};
  Gtk::Entry _filterEntry;
  Gtk::Label _groupByLabel;
  Gtk::DropDown _groupByDropdown;
  Gtk::Label _statusLabel;
  Gtk::ScrolledWindow _scrolledWindow;
  Gtk::ColumnView _columnView;
  Gtk::PopoverMenu _contextMenu;

  // Models
  Glib::RefPtr<TrackListAdapter> _adapter;
  Glib::RefPtr<Gtk::SortListModel> _sortModel;
  Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
  Glib::RefPtr<Gtk::StringList> _groupByOptions;
  Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactory;
  Glib::RefPtr<Gtk::ColumnViewColumn> _artistColumn;
  Glib::RefPtr<Gtk::ColumnViewColumn> _albumColumn;
  Glib::RefPtr<Gtk::ColumnViewColumn> _discNumberColumn;
  Glib::RefPtr<Gtk::ColumnViewColumn> _trackNumberColumn;
  Glib::RefPtr<Gtk::ColumnViewColumn> _titleColumn;
  Glib::RefPtr<Gtk::ColumnViewColumn> _tagsColumn;
  TrackPresentationSpec _presentationSpec;

  // Signals
  SelectionChangedSignal _selectionChanged;
  TrackActivatedSignal _trackActivated;
  ContextMenuRequestedSignal _contextMenuRequested;
};
