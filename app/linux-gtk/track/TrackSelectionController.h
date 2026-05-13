// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackListAdapter.h"
#include "track/TrackRowObject.h"

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk
{
  class TrackSelectionController final
  {
  public:
    using TrackId = TrackListAdapter::TrackId;
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;

    TrackSelectionController(Gtk::ColumnView& columnView,
                             TrackListAdapter& adapter,
                             Glib::RefPtr<Gtk::MultiSelection> selectionModel);

    void setupActivation();

    std::vector<TrackId> getSelectedTrackIds() const;
    std::vector<Glib::RefPtr<TrackRowObject>> getSelectedRows() const;
    std::chrono::milliseconds getSelectedTracksDuration() const;
    std::optional<TrackId> getPrimarySelectedTrackId() const;
    std::size_t selectedTrackCount() const;

    void selectTrack(TrackId trackId);
    void setPlayingTrackId(std::optional<TrackId> trackId);
    std::vector<TrackId> getVisibleTrackIds() const;

    // Exposed signals for TrackViewPage to wire to external handlers
    SelectionChangedSignal& signalSelectionChanged() { return _selectionChanged; }
    TrackActivatedSignal& signalTrackActivated() { return _trackActivated; }
    ContextMenuRequestedSignal& signalContextMenuRequested() { return _contextMenuRequested; }
    TagEditRequestedSignal& signalTagEditRequested() { return _tagEditRequested; }

  private:
    void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
    void onActivateCurrentSelection();
    std::optional<TrackId> trackIdAtPosition(std::uint32_t position) const;

    Gtk::ColumnView& _columnView;
    TrackListAdapter& _adapter;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;

    std::optional<TrackId> _playingTrackId;
    bool _suppressNextTrackActivation = false;

    sigc::connection _selectionChangedConnection;

    // Signals
    SelectionChangedSignal _selectionChanged;
    TrackActivatedSignal _trackActivated;
    ContextMenuRequestedSignal _contextMenuRequested;
    TagEditRequestedSignal _tagEditRequested;
  };
} // namespace ao::gtk
