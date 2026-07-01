// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/listitemfactory.h>
#include <sigc++/scoped_connection.h>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Gtk
{
  class ColumnView;
}

namespace ao::gtk
{
  class TrackColumnController final
  {
  public:
    using FactoryProvider = std::function<Glib::RefPtr<Gtk::ListItemFactory>(rt::TrackField)>;

    TrackColumnController(Gtk::ColumnView& columnView, uimodel::TrackColumnLayoutStore& layoutStore, ListId listId);
    ~TrackColumnController();

    TrackColumnController(TrackColumnController const&) = delete;
    TrackColumnController& operator=(TrackColumnController const&) = delete;
    TrackColumnController(TrackColumnController&&) = delete;
    TrackColumnController& operator=(TrackColumnController&&) = delete;

    // Column setup — calls factoryProvider for each presentable field
    void setupColumns(FactoryProvider const& factoryProvider);

    // Layout management
    void applyColumnLayout(std::span<rt::TrackField const> visibleFields);
    void setLayoutAndApply(std::span<rt::TrackField const> visibleFields);
    void updateColumnVisibility(std::span<rt::TrackField const> visibleFields);
    void queueSharedColumnLayoutUpdate();

    // Title position CSS variable for the playing-track beam
    void updateTitlePositionVariable();
    void queueTitlePositionVariableUpdate();

    // Synchronize visibility, layout, and CSS variables
    void syncLayout(std::span<rt::TrackField const> visibleFields);

    // Exposed for TrackViewPage to connect / manage
    Glib::RefPtr<Gtk::CssProvider> const& cssProvider() const noexcept { return _dynamicCssProviderPtr; }
    bool isTitlePositionUpdateQueuedForTest() const noexcept;
    std::string const& titlePositionCssForTest() const noexcept { return _lastTitleCss; }

  private:
    struct ColumnBinding final
    {
      rt::TrackField field = rt::TrackField::Title;
      Glib::RefPtr<Gtk::ColumnViewColumn> columnPtr;
    };

    void updateColumnExpansion(std::span<rt::TrackField const> visibleFields);
    bool flushSharedColumnLayoutUpdate();
    bool flushTitlePositionVariableUpdate();
    void updateSharedColumnLayout();
    std::vector<rt::TrackField> captureCurrentFieldOrder() const;
    std::vector<rt::TrackField> visibleFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields) const;

    void ensureColumnPosition(Glib::RefPtr<Gio::ListModel> const& columns,
                              std::size_t index,
                              Glib::RefPtr<Gtk::ColumnViewColumn> const& column);

    ColumnBinding* findColumnBinding(rt::TrackField field);
    ColumnBinding const* findColumnBinding(rt::TrackField field) const;

    ListId _listId;
    Gtk::ColumnView& _columnView;
    uimodel::TrackColumnLayoutStore& _layoutStore;

    std::vector<ColumnBinding> _columns;
    sigc::scoped_connection _queuedColumnLayoutUpdateConnection;
    sigc::scoped_connection _queuedTitlePositionUpdateConnection;
    bool _syncingColumnLayout = false;
    Glib::RefPtr<Gtk::CssProvider> _dynamicCssProviderPtr;
    std::string _lastTitleCss;
    std::vector<sigc::scoped_connection> _columnNotifyConnections;
  };
} // namespace ao::gtk
