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
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;

    TrackSelectionController(Gtk::ColumnView& columnView,
                             TrackListAdapter& adapter,
                             Glib::RefPtr<Gtk::MultiSelection> selectionModel);

    void setupActivation();

    std::vector<TrackId> getSelectedTrackIds() const noexcept;
    std::vector<Glib::RefPtr<TrackRowObject>> getSelectedRows() const noexcept;
    std::chrono::milliseconds getSelectedTracksDuration() const noexcept;
    std::optional<TrackId> getPrimarySelectedTrackId() const noexcept;
    std::size_t selectedTrackCount() const noexcept;

    void selectTrack(TrackId trackId);
    void scrollToTrack(TrackId trackId);
    void setPlayingTrackId(std::optional<TrackId> optTrackId);
    std::vector<TrackId> getVisibleTrackIds() const noexcept;

    // Exposed signals for TrackViewPage to wire to external handlers
    SelectionChangedSignal& signalSelectionChanged() noexcept { return _selectionChanged; }
    TrackActivatedSignal& signalTrackActivated() noexcept { return _trackActivated; }
    ContextMenuRequestedSignal& signalContextMenuRequested() noexcept { return _contextMenuRequested; }
    TagEditRequestedSignal& signalTagEditRequested() noexcept { return _tagEditRequested; }

  private:
    void onSelectionChanged(std::uint32_t position, std::uint32_t nItems);
    void onActivateCurrentSelection();
    std::optional<TrackId> trackIdAtPosition(std::uint32_t position) const noexcept;

    Gtk::ColumnView& _columnView;
    TrackListAdapter& _adapter;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;

    std::optional<TrackId> _optPlayingTrackId;
    bool _suppressNextTrackActivation = false;

    sigc::scoped_connection _selectionChangedConnection;

    // Signals
    SelectionChangedSignal _selectionChanged;
    TrackActivatedSignal _trackActivated;
    ContextMenuRequestedSignal _contextMenuRequested;
    TagEditRequestedSignal _tagEditRequested;
  };
} // namespace ao::gtk
