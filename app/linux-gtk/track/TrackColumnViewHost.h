// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackColumnController.h"
#include "track/TrackSelectionController.h"

#include <gtkmm.h>

#include <memory>

namespace ao::gtk
{
  class TrackListAdapter;

  class TrackColumnViewHost final
  {
  public:
    using FactoryProvider = TrackColumnController::FactoryProvider;

    TrackColumnViewHost(TrackListAdapter& adapter,
                        TrackColumnLayoutModel& columnLayoutModel,
                        Glib::RefPtr<Gtk::MultiSelection> const& selectionModel);
    ~TrackColumnViewHost();

    Gtk::ColumnView& columnView() { return *_columnView; }
    Gtk::ColumnView const& columnView() const { return *_columnView; }

    TrackColumnController& columnController() { return *_columnController; }
    TrackSelectionController& selectionController() { return *_selectionController; }

    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const;

    void setupColumns(FactoryProvider const& factoryProvider);
    void setupSelectionActivation();

    // Signal accessors — stable across rebuilds
    TrackSelectionController::SelectionChangedSignal& signalSelectionChanged() { return _selectionChangedSignal; }
    TrackSelectionController::TrackActivatedSignal& signalTrackActivated() { return _trackActivatedSignal; }
    TrackSelectionController::ContextMenuRequestedSignal& signalContextMenuRequested()
    {
      return _contextMenuRequestedSignal;
    }
    TrackSelectionController::TagEditRequestedSignal& signalTagEditRequested() { return _tagEditRequestedSignal; }

    // Build a new ColumnView generation off-tree and return it.
    // The old generation is retired. Caller swaps the scrolled-window child.
    Gtk::ColumnView& rebuild(TrackListAdapter& adapter,
                             TrackColumnLayoutModel& columnLayoutModel,
                             Glib::RefPtr<Gtk::MultiSelection> const& selectionModel,
                             FactoryProvider const& factoryProvider);

  private:
    void connectSelectionSignals();

    std::unique_ptr<Gtk::ColumnView> _columnView;
    std::unique_ptr<TrackColumnController> _columnController;
    std::unique_ptr<TrackSelectionController> _selectionController;

    // Internal forwarding connections - must be scoped to prevent dangling pointers
    sigc::scoped_connection _selectionChangedConn;
    sigc::scoped_connection _trackActivatedConn;
    sigc::scoped_connection _contextMenuRequestedConn;
    sigc::scoped_connection _tagEditRequestedConn;

    // Stable signals — reconnected to new selection controller on rebuild
    TrackSelectionController::SelectionChangedSignal _selectionChangedSignal;
    TrackSelectionController::TrackActivatedSignal _trackActivatedSignal;
    TrackSelectionController::ContextMenuRequestedSignal _contextMenuRequestedSignal;
    TrackSelectionController::TagEditRequestedSignal _tagEditRequestedSignal;
  };
} // namespace ao::gtk
