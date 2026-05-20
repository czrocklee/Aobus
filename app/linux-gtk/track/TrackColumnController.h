// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/TrackField.h"
#include "track/TrackPresentation.h"

#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/listitemfactory.h>
#include <sigc++/scoped_connection.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ao::gtk
{
  class TrackColumnController final
  {
  public:
    using FactoryProvider = std::function<Glib::RefPtr<Gtk::ListItemFactory>(rt::TrackField)>;

    TrackColumnController(Gtk::ColumnView& columnView, TrackColumnLayoutModel& layoutModel);

    // Column setup — calls factoryProvider for each presentable field
    void setupColumns(FactoryProvider const& factoryProvider);

    // Layout management
    void applyColumnLayout(std::span<rt::TrackField const> visibleFields);
    void setLayoutAndApply(std::span<rt::TrackField const> visibleFields);
    void updateColumnVisibility(std::span<rt::TrackField const> visibleFields);
    void queueSharedColumnLayoutUpdate();

    // Title position CSS variable for the playing-track beam
    void updateTitlePositionVariable();

    // Synchronize visibility, layout, and CSS variables
    void syncLayout(std::span<rt::TrackField const> visibleFields);

    // Exposed for TrackViewPage to connect / manage
    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const noexcept { return _dynamicCssProvider; }

  private:
    struct ColumnBinding final
    {
      rt::TrackField field = rt::TrackField::Title;
      Glib::RefPtr<Gtk::ColumnViewColumn> column;
      std::int32_t defaultWidth = -1;
    };

    void updateColumnExpansion(std::span<rt::TrackField const> visibleFields);
    bool flushSharedColumnLayoutUpdate();
    void updateSharedColumnLayout();
    std::vector<rt::TrackField> captureCurrentFieldOrder() const;
    std::vector<rt::TrackField> visibleFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields) const;

    void ensureColumnPosition(Glib::RefPtr<Gio::ListModel> const& columns,
                              std::size_t idx,
                              Glib::RefPtr<Gtk::ColumnViewColumn> const& column);

    ColumnBinding* findColumnBinding(rt::TrackField field);
    ColumnBinding const* findColumnBinding(rt::TrackField field) const;

    Gtk::ColumnView& _columnView;
    TrackColumnLayoutModel& _columnLayoutModel;

    std::vector<ColumnBinding> _columns;
    sigc::scoped_connection _queuedColumnLayoutUpdateConnection;
    sigc::scoped_connection _layoutChangedConnection;
    bool _syncingColumnLayout = false;
    bool _capturingColumnLayout = false;
    Glib::RefPtr<Gtk::CssProvider> _dynamicCssProvider;
    std::vector<sigc::scoped_connection> _columnNotifyConnections;
  };
} // namespace ao::gtk
