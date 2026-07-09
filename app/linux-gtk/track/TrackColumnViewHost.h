// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackColumnController.h"
#include "track/TrackSelectionController.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <glibmm/refptr.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/multiselection.h>
#include <sigc++/scoped_connection.h>

#include <memory>

namespace Gtk
{
  class ColumnView;
}

namespace ao::gtk
{
  class TrackListModel;

  class TrackColumnViewHost final
  {
  public:
    using FactoryProvider = TrackColumnController::FactoryProvider;

    TrackColumnViewHost(Glib::RefPtr<TrackListModel> modelPtr,
                        uimodel::TrackColumnLayoutStore& layoutStore,
                        Glib::RefPtr<Gtk::MultiSelection> const& selectionModelPtr,
                        ListId listId);
    ~TrackColumnViewHost();

    // Not copyable or movable
    TrackColumnViewHost(TrackColumnViewHost const&) = delete;
    TrackColumnViewHost& operator=(TrackColumnViewHost const&) = delete;
    TrackColumnViewHost(TrackColumnViewHost&&) = delete;
    TrackColumnViewHost& operator=(TrackColumnViewHost&&) = delete;

    Gtk::ColumnView& columnView() { return *_columnViewPtr; }
    Gtk::ColumnView const& columnView() const { return *_columnViewPtr; }

    TrackColumnController& columnController() { return *_columnControllerPtr; }
    TrackSelectionController& selectionController() { return *_selectionControllerPtr; }

    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const;

    void configureColumns(FactoryProvider const& factoryProvider);
    void configureSelectionActivation();

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
    Gtk::ColumnView& rebuild(Glib::RefPtr<TrackListModel> modelPtr,
                             uimodel::TrackColumnLayoutStore& layoutStore,
                             Glib::RefPtr<Gtk::MultiSelection> const& selectionModelPtr,
                             FactoryProvider const& factoryProvider,
                             ListId listId);

  private:
    void connectSelectionSignals();

    std::unique_ptr<Gtk::ColumnView> _columnViewPtr;
    std::unique_ptr<TrackColumnController> _columnControllerPtr;
    std::unique_ptr<TrackSelectionController> _selectionControllerPtr;

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
