// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>

#include <algorithm>
#include <format>
#include <ranges>

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

      _columnNotifyConnections.push_back(column->property_fixed_width().signal_changed().connect(
        [this]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        }));

      _columnNotifyConnections.push_back(column->property_fixed_width().signal_changed().connect(
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
        if (!binding)
        {
          continue;
        }

        // Check if the column is already at the target index
        bool needsMove = true;
        if (columns->get_n_items() > idx)
        {
          if (auto currentObj = columns->get_object(static_cast<::guint>(idx)))
          {
            if (currentObj == binding->column)
            {
              needsMove = false;
            }
          }
        }

        if (needsMove)
        {
          // Check if column is already in the view elsewhere

          for (::guint i = 0; i < columns->get_n_items(); ++i)
          {
            if (columns->get_object(i) == binding->column)
            {
              _columnView.remove_column(binding->column);
              break;
            }
          }

          _columnView.insert_column(static_cast<::guint>(idx), binding->column);
        }

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

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      auto const object = columns->get_object(i);
      auto const column = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);

      if (!column)
      {
        continue;
      }

      auto const columnId = trackColumnFromId(std::string{column->get_id()});

      if (!columnId)
      {
        continue;
      }

      auto state = currentStateFor(*columnId);

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

    for (std::uint32_t i = 0; i < columns->get_n_items(); ++i)
    {
      auto const col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(i));

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
