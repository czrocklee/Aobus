// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>

#include <glibmm/refptr.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Gtk
{
  class ColumnView;
}

namespace ao::gtk
{
  class TrackSelectionController final
  {
  public:
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;

    TrackSelectionController(Gtk::ColumnView& columnView,
                             Glib::RefPtr<TrackListModel> modelPtr,
                             Glib::RefPtr<Gtk::MultiSelection> selectionModelPtr);

    void configureActivation();

    std::vector<TrackId> selectedTrackIds() const noexcept;
    std::vector<Glib::RefPtr<TrackRowObject>> selectedRows() const noexcept;
    std::chrono::milliseconds selectedTracksDuration() const noexcept;
    TrackId primarySelectedTrackId() const noexcept;
    std::size_t selectedTrackCount() const noexcept;

    void selectTrack(TrackId trackId);
    void scrollToTrack(TrackId trackId);
    void setPlayingTrackId(TrackId trackId);
    std::vector<TrackId> visibleTrackIds() const noexcept;

    // Exposed signals for TrackViewPage to wire to external handlers
    SelectionChangedSignal& signalSelectionChanged() noexcept { return _selectionChanged; }
    TrackActivatedSignal& signalTrackActivated() noexcept { return _trackActivated; }
    ContextMenuRequestedSignal& signalContextMenuRequested() noexcept { return _contextMenuRequested; }
    TagEditRequestedSignal& signalTagEditRequested() noexcept { return _tagEditRequested; }

  private:
    void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
    void onActivateCurrentSelection();
    TrackId trackIdAtPosition(std::uint32_t position) const noexcept;

    Gtk::ColumnView& _columnView;
    Glib::RefPtr<TrackListModel> _modelPtr;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModelPtr;

    TrackId _playingTrackId{kInvalidTrackId};
    bool _suppressNextTrackActivation = false;

    sigc::scoped_connection _selectionChangedConnection;

    // Signals
    SelectionChangedSignal _selectionChanged;
    TrackActivatedSignal _trackActivated;
    ContextMenuRequestedSignal _contextMenuRequested;
    TagEditRequestedSignal _tagEditRequested;
  };
} // namespace ao::gtk
