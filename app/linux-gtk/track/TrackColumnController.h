// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/ColumnVisibilityModel.h"
#include "track/TrackPresentation.h"

#include <gtkmm.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ao::gtk
{
  class TrackColumnController final
  {
  public:
    using FactoryProvider = std::function<Glib::RefPtr<Gtk::SignalListItemFactory>(TrackColumnDefinition const&)>;

    TrackColumnController(Gtk::ColumnView& columnView, TrackColumnLayoutModel& layoutModel);

    Glib::RefPtr<ColumnVisibilityModel> visibilityModel() const noexcept { return _visibilityModel; }

    // Column setup — calls factoryProvider for each column definition
    void setupColumns(FactoryProvider const& factoryProvider);

    // Layout management
    void applyColumnLayout();
    void setLayoutAndApply(TrackColumnLayout const& layout);
    void updateColumnVisibility();
    void queueSharedColumnLayoutUpdate();

    // Title position CSS variable for the playing-track beam
    void updateTitlePositionVariable();

    // Synchronize visibility, layout, and CSS variables
    void syncLayout();

    // Exposed for TrackViewPage to connect / manage
    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const noexcept { return _dynamicCssProvider; }

  private:
    struct ColumnBinding final
    {
      TrackColumn id;
      Glib::RefPtr<Gtk::ColumnViewColumn> column;
      std::int32_t defaultWidth = -1;
    };

    void updateColumnExpansion(TrackColumnLayout const& layout);
    bool flushSharedColumnLayoutUpdate();
    void updateSharedColumnLayout();
    TrackColumnLayout captureCurrentColumnLayout() const;

    ColumnBinding* findColumnBinding(TrackColumn column);
    ColumnBinding const* findColumnBinding(TrackColumn column) const;

    Gtk::ColumnView& _columnView;
    TrackColumnLayoutModel& _columnLayoutModel;

    std::vector<ColumnBinding> _columns;
    sigc::scoped_connection _queuedColumnLayoutUpdateConnection;
    sigc::scoped_connection _layoutChangedConnection;
    bool _syncingColumnLayout = false;
    bool _capturingColumnLayout = false;
    Glib::RefPtr<Gtk::CssProvider> _dynamicCssProvider;
    Glib::RefPtr<ColumnVisibilityModel> _visibilityModel;
    std::vector<sigc::scoped_connection> _columnNotifyConnections;
  };
} // namespace ao::gtk
