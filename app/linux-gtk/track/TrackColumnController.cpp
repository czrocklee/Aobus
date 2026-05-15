// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"
#include "track/ColumnVisibilityModel.h"
#include "track/TrackPresentation.h"

#include <giomm/listmodel.h>
#include <glibmm/binding.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <sigc++/functors/mem_fun.h>
#include <glib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <string>

namespace ao::gtk
{
  namespace
  {
    constexpr double kTitlePositionDivisor = 2.0;
  }

  TrackColumnController::TrackColumnController(Gtk::ColumnView& columnView, TrackColumnLayoutModel& layoutModel)
    : _columnView{columnView}, _columnLayoutModel{layoutModel}
  {
    _dynamicCssProvider = Gtk::CssProvider::create();
    _visibilityModel = ColumnVisibilityModel::create();

    _layoutChangedConnection =
      _columnLayoutModel.signalChanged().connect(sigc::mem_fun(*this, &TrackColumnController::updateColumnVisibility));
  }

  void TrackColumnController::setupColumns(FactoryProvider const& factoryProvider)
  {
    _columns.reserve(trackColumnDefinitions().size());

    for (auto const& definition : trackColumnDefinitions())
    {
      auto const title = Glib::ustring{std::string{definition.title}};
      auto const column = Gtk::ColumnViewColumn::create(title, factoryProvider(definition));

      column->set_id(Glib::ustring{std::string{definition.id}});
      column->set_expand(definition.expands);
      column->set_resizable(true);
      column->set_fixed_width(definition.defaultWidth);

      _columnNotifyConnections.emplace_back(column->property_fixed_width().signal_changed().connect(
        [this]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        }));

      _columnNotifyConnections.emplace_back(column->property_fixed_width().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable)));

      _columnView.append_column(column);

      Glib::Binding::bind_property(_visibilityModel->property_visible(definition.column),
                                   column->property_visible(),
                                   Glib::Binding::Flags::SYNC_CREATE);

      _columns.push_back({.id = definition.column, .column = column, .defaultWidth = definition.defaultWidth});
    }
  }

  void TrackColumnController::applyColumnLayout()
  {
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    _syncingColumnLayout = true;
    _layoutChangedConnection.block(); // Block visibility updates during structural changes

    auto const columns = _columnView.get_columns();

    if (columns)
    {
      for (auto const& [idx, state] : std::views::enumerate(layout.columns))
      {
        auto* const binding = findColumnBinding(state.column);

        if (binding == nullptr)
        {
          continue;
        }

        ensureColumnPosition(columns, idx, binding->column);

        auto const width = state.width == -1 ? binding->defaultWidth : state.width;

        if (binding->column->get_fixed_width() != width)
        {
          binding->column->set_fixed_width(width);
        }
      }
    }

    _layoutChangedConnection.unblock(); // Unblock before manual update

    updateColumnExpansion(layout);
    updateColumnVisibility();

    _syncingColumnLayout = false;
  }

  void TrackColumnController::ensureColumnPosition(Glib::RefPtr<Gio::ListModel> const& columns,
                                                   std::size_t idx,
                                                   Glib::RefPtr<Gtk::ColumnViewColumn> const& column)
  {
    bool needsMove = true;

    if (columns->get_n_items() > idx)
    {
      if (auto currentObj = columns->get_object(static_cast<::guint>(idx)))
      {
        if (currentObj == column)
        {
          needsMove = false;
        }
      }
    }

    if (needsMove)
    {
      // Check if column is already in the view elsewhere
      for (::guint loopIdx = 0; loopIdx < columns->get_n_items(); ++loopIdx)
      {
        if (columns->get_object(loopIdx) == column)
        {
          _columnView.remove_column(column);
          break;
        }
      }

      _columnView.insert_column(static_cast<::guint>(idx), column);
    }
  }

  void TrackColumnController::setLayoutAndApply(TrackColumnLayout const& layout)
  {
    _syncingColumnLayout = true;
    _layoutChangedConnection.block();
    _columnLayoutModel.setLayout(layout);
    _layoutChangedConnection.unblock();
    _syncingColumnLayout = false;

    applyColumnLayout();
  }

  void TrackColumnController::updateColumnExpansion(TrackColumnLayout const& layout)
  {
    auto const expandingColumns = expandingTrackColumnsForLayout(layout);

    for (auto& binding : _columns)
    {
      auto const shouldExpand = std::ranges::contains(expandingColumns, binding.id);

      if (binding.column->get_expand() != shouldExpand)
      {
        binding.column->set_expand(shouldExpand);
      }
    }
  }

  void TrackColumnController::queueSharedColumnLayoutUpdate()
  {
    if (_queuedColumnLayoutUpdateConnection.connected())
    {
      return;
    }

    _queuedColumnLayoutUpdateConnection =
      Glib::signal_idle().connect(sigc::mem_fun(*this, &TrackColumnController::flushSharedColumnLayoutUpdate));
  }

  bool TrackColumnController::flushSharedColumnLayoutUpdate()
  {
    _queuedColumnLayoutUpdateConnection.disconnect();

    if (_syncingColumnLayout)
    {
      return false;
    }

    updateSharedColumnLayout();

    return false;
  }

  void TrackColumnController::updateSharedColumnLayout()
  {
    _capturingColumnLayout = true;
    _columnLayoutModel.setLayout(captureCurrentColumnLayout());
    _capturingColumnLayout = false;
  }

  TrackColumnLayout TrackColumnController::captureCurrentColumnLayout() const
  {
    auto layout = TrackColumnLayout{};
    auto currentLayout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    auto const currentStateFor = [&currentLayout](TrackColumn column)
    {
      auto const it = std::ranges::find(currentLayout.columns, column, &TrackColumnState::column);

      return it != currentLayout.columns.end() ? *it : TrackColumnState{.column = column};
    };

    auto const columns = _columnView.get_columns();

    if (!columns)
    {
      return currentLayout;
    }

    auto const nItems = columns->get_n_items();

    layout.columns.reserve(nItems);

    for (std::uint32_t idx = 0; idx < nItems; ++idx)
    {
      auto const object = columns->get_object(idx);
      auto const column = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);

      if (!column)
      {
        continue;
      }

      auto const optColumnId = trackColumnFromId(std::string{column->get_id()});

      if (!optColumnId)
      {
        continue;
      }

      auto state = currentStateFor(*optColumnId);

      state.width = column->get_fixed_width();
      layout.columns.push_back(state);
    }

    return normalizeTrackColumnLayout(layout);
  }

  void TrackColumnController::updateColumnVisibility()
  {
    _visibilityModel->recompute(normalizeTrackColumnLayout(_columnLayoutModel.layout()));
  }

  void TrackColumnController::syncLayout()
  {
    updateColumnVisibility();
    applyColumnLayout();
    updateTitlePositionVariable();
  }

  void TrackColumnController::updateTitlePositionVariable()
  {
    double titleX = 0;
    bool found = false;

    auto const columns = _columnView.get_columns();

    if (!columns)
    {
      return;
    }

    for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
    {
      auto const col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(idx));

      if (!col || !col->get_visible())
      {
        continue;
      }

      if (col->get_title() == "Title")
      {
        titleX += col->get_fixed_width() / kTitlePositionDivisor;
        found = true;
        break;
      }

      titleX += col->get_fixed_width();
    }

    if (found)
    {
      auto const css = std::format("columnview {{ --ao-title-x: {:.1f}px; }}", titleX);

      _dynamicCssProvider->load_from_data(css);
    }
  }

  TrackColumnController::ColumnBinding* TrackColumnController::findColumnBinding(TrackColumn column)
  {
    auto const it = std::ranges::find(_columns, column, &ColumnBinding::id);

    return it != _columns.end() ? &*it : nullptr;
  }

  TrackColumnController::ColumnBinding const* TrackColumnController::findColumnBinding(TrackColumn column) const
  {
    auto const it = std::ranges::find(_columns, column, &ColumnBinding::id);

    return it != _columns.end() ? &*it : nullptr;
  }
} // namespace ao::gtk
