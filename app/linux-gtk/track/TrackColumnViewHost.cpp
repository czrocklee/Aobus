// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnViewHost.h"
#include "track/TrackColumnController.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackSelectionController.h"
#include <ao/Type.h>

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
  TrackColumnViewHost::TrackColumnViewHost(TrackListAdapter& adapter,
                                           TrackColumnLayoutModel& columnLayoutModel,
                                           Glib::RefPtr<Gtk::MultiSelection> const& selectionModel)
    : _columnView{std::make_unique<Gtk::ColumnView>()}
    , _columnController{std::make_unique<TrackColumnController>(*_columnView, columnLayoutModel)}
    , _selectionController{std::make_unique<TrackSelectionController>(*_columnView, adapter, selectionModel)}
  {
    connectSelectionSignals();
  }

  TrackColumnViewHost::~TrackColumnViewHost() = default;

  Glib::RefPtr<Gtk::CssProvider> const& TrackColumnViewHost::cssProvider() const
  {
    return _columnController->cssProvider();
  }

  void TrackColumnViewHost::setupColumns(FactoryProvider const& factoryProvider)
  {
    _columnController->setupColumns(factoryProvider);
  }

  void TrackColumnViewHost::setupSelectionActivation()
  {
    _selectionController->setupActivation();
  }

  void TrackColumnViewHost::connectSelectionSignals()
  {
    _selectionChangedConn =
      _selectionController->signalSelectionChanged().connect([this] { _selectionChangedSignal.emit(); });
    _trackActivatedConn =
      _selectionController->signalTrackActivated().connect([this](TrackId id) { _trackActivatedSignal.emit(id); });
    _contextMenuRequestedConn = _selectionController->signalContextMenuRequested().connect(
      [this](double xPos, double yPos) { _contextMenuRequestedSignal.emit(xPos, yPos); });
    _tagEditRequestedConn = _selectionController->signalTagEditRequested().connect(
      [this](std::vector<TrackId> const& ids, Gtk::Widget* widget) { _tagEditRequestedSignal.emit(ids, widget); });
  }

  Gtk::ColumnView& TrackColumnViewHost::rebuild(TrackListAdapter& adapter,
                                                TrackColumnLayoutModel& columnLayoutModel,
                                                Glib::RefPtr<Gtk::MultiSelection> const& selectionModel,
                                                FactoryProvider const& factoryProvider)
  {
    auto newView = std::make_unique<Gtk::ColumnView>();
    auto newSelection = std::make_unique<TrackSelectionController>(*newView, adapter, selectionModel);
    auto newColumn = std::make_unique<TrackColumnController>(*newView, columnLayoutModel);

    // Retire old generation
    _columnController = std::move(newColumn);
    _selectionController = std::move(newSelection);
    _columnView = std::move(newView);

    // Wire factories for the new columns
    _columnController->setupColumns(factoryProvider);

    // Reconnect forwarding signals to the new selection controller
    connectSelectionSignals();

    return *_columnView;
  }
} // namespace ao::gtk
