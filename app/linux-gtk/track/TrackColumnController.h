// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackPresentation.h"

#include <gtkmm.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ao::gtk
{
  class TrackColumnController final
  {
  public:
    using FactoryProvider = std::function<Glib::RefPtr<Gtk::SignalListItemFactory>(TrackColumnDefinition const&)>;
    using RedundancyProvider = std::function<std::unordered_set<TrackColumn>()>;

    TrackColumnController(Gtk::ColumnView& columnView, TrackColumnLayoutModel& layoutModel);

    void setRedundancyProvider(RedundancyProvider provider) { _redundancyProvider = std::move(provider); }

    // Column setup — calls factoryProvider for each column definition
    void setupColumns(FactoryProvider const& factoryProvider);
    void setupColumnControls();

    // Layout management
    void applyColumnLayout();
    void updateColumnVisibility();
    void queueSharedColumnLayoutUpdate();

    // Title position CSS variable for the playing-track beam
    void updateTitlePositionVariable();

    // Exposed for TrackViewPage to place in layout
    Gtk::MenuButton& columnsButton() { return _columnsButton; }

    // Exposed for TrackViewPage to connect / manage
    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const { return _dynamicCssProvider; }

  private:
    struct ColumnBinding final
    {
      TrackColumn id;
      Glib::RefPtr<Gtk::ColumnViewColumn> column;
      Gtk::CheckButton* toggle = nullptr;
      std::int32_t defaultWidth = -1;
    };

    void syncColumnToggleStates();
    bool flushSharedColumnLayoutUpdate();
    void updateSharedColumnLayout();
    TrackColumnLayout captureCurrentColumnLayout() const;

    ColumnBinding* findColumnBinding(TrackColumn column);
    ColumnBinding const* findColumnBinding(TrackColumn column) const;

    Gtk::ColumnView& _columnView;
    TrackColumnLayoutModel& _columnLayoutModel;
    Glib::RefPtr<Gio::ListModel> _columnModel;

    Gtk::MenuButton _columnsButton;
    Gtk::Popover _columnsPopover;
    Gtk::Box _columnsPopoverBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _columnsPopoverTitle;
    Gtk::ListBox _columnToggleList;
    Gtk::Separator _columnsPopoverSeparator;
    Gtk::Button _resetColumnsButton;

    std::vector<ColumnBinding> _columns;
    sigc::connection _columnLayoutChangedConnection;
    sigc::connection _columnModelChangedConnection;
    sigc::connection _queuedColumnLayoutUpdateConnection;
    sigc::connection _columnVisibilityIdle;
    bool _syncingColumnLayout = false;
    bool _capturingColumnLayout = false;
    Glib::RefPtr<Gtk::CssProvider> _dynamicCssProvider;
    std::vector<sigc::connection> _columnNotifyConnections;
    RedundancyProvider _redundancyProvider;
  };
} // namespace ao::gtk
