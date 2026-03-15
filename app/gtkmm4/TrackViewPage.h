#pragma once

#include "TrackListAdapter.h"

#include <gtkmm.h>

#include <cstdint>
#include <vector>

class TrackViewPage : public Gtk::Box
{
public:
  explicit TrackViewPage(Glib::RefPtr<TrackListAdapter> const& adapter);
  ~TrackViewPage() override;

  // Get the selected track IDs
  std::vector<TrackListAdapter::TrackId> getSelectedTrackIds() const;

  // Get the column view for context menu setup
  Gtk::ColumnView& getColumnView() { return _columnView; }

  // Signal for selection changes
  using SelectionChangedSignal = sigc::signal<void()>;
  SelectionChangedSignal& signalSelectionChanged();

private:
  // Child widgets
  Gtk::Entry _filterEntry;
  Gtk::ScrolledWindow _scrolledWindow;
  Gtk::ColumnView _columnView;

  // Models
  Glib::RefPtr<TrackListAdapter> _adapter;
  Glib::RefPtr<Gtk::MultiSelection> _selectionModel;

  // Signal
  SelectionChangedSignal _selectionChanged;

  // Setup methods
  void setupColumns();
  void onFilterChanged();
  void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
};
