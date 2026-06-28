// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnViewHost.h"

#include "track/TrackColumnController.h"
#include "track/TrackListModel.h"
#include "track/TrackSelectionController.h"
#include <ao/Type.h>
#include <ao/uimodel/track/TrackColumnLayoutStore.h>

#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/widget.h>

#include <memory>
#include <utility>
#include <vector>

namespace ao::gtk
{
  TrackColumnViewHost::TrackColumnViewHost(Glib::RefPtr<TrackListModel> modelPtr,
                                           uimodel::track::TrackColumnLayoutStore& layoutStore,
                                           Glib::RefPtr<Gtk::MultiSelection> const& selectionModel,
                                           ao::ListId listId)
    : _columnViewPtr{std::make_unique<Gtk::ColumnView>()}
    , _columnControllerPtr{std::make_unique<TrackColumnController>(*_columnViewPtr, layoutStore, listId)}
    , _selectionControllerPtr{std::make_unique<TrackSelectionController>(*_columnViewPtr, modelPtr, selectionModel)}
  {
    connectSelectionSignals();
  }

  TrackColumnViewHost::~TrackColumnViewHost() = default;

  Glib::RefPtr<Gtk::CssProvider> const& TrackColumnViewHost::cssProvider() const
  {
    return _columnControllerPtr->cssProvider();
  }

  void TrackColumnViewHost::setupColumns(FactoryProvider const& factoryProvider)
  {
    _columnControllerPtr->setupColumns(factoryProvider);
  }

  void TrackColumnViewHost::setupSelectionActivation()
  {
    _selectionControllerPtr->setupActivation();
  }

  void TrackColumnViewHost::connectSelectionSignals()
  {
    _selectionChangedConn =
      _selectionControllerPtr->signalSelectionChanged().connect([this] { _selectionChangedSignal.emit(); });
    _trackActivatedConn =
      _selectionControllerPtr->signalTrackActivated().connect([this](TrackId id) { _trackActivatedSignal.emit(id); });
    _contextMenuRequestedConn = _selectionControllerPtr->signalContextMenuRequested().connect(
      [this](double xPos, double yPos) { _contextMenuRequestedSignal.emit(xPos, yPos); });
    _tagEditRequestedConn = _selectionControllerPtr->signalTagEditRequested().connect(
      [this](std::vector<TrackId> const& ids, Gtk::Widget* widget) { _tagEditRequestedSignal.emit(ids, widget); });
  }

  Gtk::ColumnView& TrackColumnViewHost::rebuild(Glib::RefPtr<TrackListModel> modelPtr,
                                                uimodel::track::TrackColumnLayoutStore& layoutStore,
                                                Glib::RefPtr<Gtk::MultiSelection> const& selectionModel,
                                                FactoryProvider const& factoryProvider,
                                                ao::ListId listId)
  {
    auto newViewPtr = std::make_unique<Gtk::ColumnView>();
    auto newSelectionPtr = std::make_unique<TrackSelectionController>(*newViewPtr, modelPtr, selectionModel);
    auto newColumnPtr = std::make_unique<TrackColumnController>(*newViewPtr, layoutStore, listId);

    // Retire old generation
    _columnControllerPtr = std::move(newColumnPtr);
    _selectionControllerPtr = std::move(newSelectionPtr);
    _columnViewPtr = std::move(newViewPtr);

    // Wire factories for the new columns
    _columnControllerPtr->setupColumns(factoryProvider);

    // Reconnect forwarding signals to the new selection controller
    connectSelectionSignals();

    return *_columnViewPtr;
  }
} // namespace ao::gtk
