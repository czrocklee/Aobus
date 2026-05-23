// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "runtime/TrackField.h"
#include "track/TrackFieldUi.h"
#include "track/TrackPresentation.h"

#include <giomm/listmodel.h>
#include <glib.h>
#include <glibmm/binding.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

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

    _layoutChangedConnection = _columnLayoutModel.signalChanged().connect([this] { queueSharedColumnLayoutUpdate(); });
  }

  void TrackColumnController::setupColumns(FactoryProvider const& factoryProvider)
  {
    auto const defs = rt::trackFieldDefinitions();

    _columns.reserve(defs.size());

    for (auto const& rtDef : defs)
    {
      if (!rtDef.presentable)
      {
        continue;
      }

      auto const title = Glib::ustring{rtDef.label.data(), rtDef.label.size()};
      auto const column = Gtk::ColumnViewColumn::create(title, factoryProvider(rtDef.field));
      auto const* uiDef = trackFieldUiDefinition(rtDef.field);

      column->set_id(Glib::ustring{rtDef.id.data(), rtDef.id.size()});

      column->set_resizable(true);

      if (uiDef != nullptr)
      {
        column->set_expand(uiDef->columnExpands);
        column->set_fixed_width(uiDef->defaultColumnWidth);
      }

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

      _columns.push_back(
        {.field = rtDef.field, .column = column, .defaultWidth = uiDef != nullptr ? uiDef->defaultColumnWidth : -1});
    }
  }

  void TrackColumnController::applyColumnLayout(std::span<rt::TrackField const> visibleFields)
  {
    _syncingColumnLayout = true;
    _layoutChangedConnection.block();

    if (auto const columns = _columnView.get_columns(); columns)
    {
      // Reorder columns to match visibleFields order
      for (auto const& [idx, field] : std::views::enumerate(visibleFields))
      {
        auto* const binding = findColumnBinding(field);

        if (binding == nullptr)
        {
          continue;
        }

        ensureColumnPosition(columns, idx, binding->column);

        auto const width = _columnLayoutModel.state().widths.at(static_cast<std::size_t>(field));
        auto const effectiveWidth = (width == 0 ? binding->defaultWidth : width);

        if (binding->column->get_fixed_width() != effectiveWidth)
        {
          binding->column->set_fixed_width(effectiveWidth);
        }
      }
    }

    _layoutChangedConnection.unblock();

    updateColumnExpansion(visibleFields);
    updateColumnVisibility(visibleFields);

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

  void TrackColumnController::setLayoutAndApply(std::span<rt::TrackField const> visibleFields)
  {
    auto const orderedFields = visibleFieldsInStoredOrder(visibleFields);
    applyColumnLayout(orderedFields);
  }

  void TrackColumnController::updateColumnExpansion(std::span<rt::TrackField const> visibleFields)
  {
    auto expanding = rt::TrackField::Tags;

    for (auto const field : visibleFields)
    {
      if (fieldIsExpanding(field))
      {
        expanding = field;
        break;
      }
    }

    if (!std::ranges::contains(visibleFields, expanding))
    {
      // Fallback: first visible field
      if (auto const titleField = rt::TrackField::Title; std::ranges::contains(visibleFields, titleField))
      {
        expanding = titleField;
      }
      else if (!visibleFields.empty())
      {
        expanding = visibleFields.front();
      }
    }

    for (auto& binding : _columns)
    {
      if (auto const shouldExpand = binding.field == expanding; binding.column->get_expand() != shouldExpand)
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

    auto state = TrackColumnViewState{};

    if (auto const columns = _columnView.get_columns(); columns)
    {
      for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
      {
        auto const obj = columns->get_object(idx);
        auto const gtkColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(obj);

        if (!gtkColumn)
        {
          continue;
        }

        auto const optField = rt::trackFieldFromId(gtkColumn->get_id().raw());

        if (!optField)
        {
          continue;
        }

        state.widths.at(static_cast<std::size_t>(*optField)) = gtkColumn->get_fixed_width();
      }
    }

    state.fieldOrder = captureCurrentFieldOrder();

    _columnLayoutModel.setState(state);
    _capturingColumnLayout = false;
  }

  std::vector<rt::TrackField> TrackColumnController::captureCurrentFieldOrder() const
  {
    auto fields = std::vector<rt::TrackField>{};
    auto const columns = _columnView.get_columns();

    if (!columns)
    {
      return fields;
    }

    fields.reserve(columns->get_n_items());

    for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
    {
      auto const obj = columns->get_object(idx);
      auto const gtkColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(obj);

      if (!gtkColumn)
      {
        continue;
      }

      if (auto const optField = rt::trackFieldFromId(std::string{gtkColumn->get_id()}); optField)
      {
        fields.push_back(*optField);
      }
    }

    return fields;
  }

  std::vector<rt::TrackField> TrackColumnController::visibleFieldsInStoredOrder(
    std::span<rt::TrackField const> visibleFields) const
  {
    auto ordered = std::vector<rt::TrackField>{};
    ordered.reserve(visibleFields.size());

    auto const appendIfVisible = [&ordered, visibleFields](rt::TrackField field)
    {
      if (!std::ranges::contains(visibleFields, field) || std::ranges::contains(ordered, field))
      {
        return;
      }

      ordered.push_back(field);
    };

    for (auto const field : _columnLayoutModel.state().fieldOrder)
    {
      appendIfVisible(field);
    }

    for (auto const field : visibleFields)
    {
      appendIfVisible(field);
    }

    return ordered;
  }

  void TrackColumnController::updateColumnVisibility(std::span<rt::TrackField const> visibleFields)
  {
    for (auto& binding : _columns)
    {
      auto const visible = std::ranges::contains(visibleFields, binding.field);
      binding.column->set_visible(visible);
    }
  }

  void TrackColumnController::syncLayout(std::span<rt::TrackField const> visibleFields)
  {
    auto const orderedFields = visibleFieldsInStoredOrder(visibleFields);
    updateColumnVisibility(orderedFields);
    applyColumnLayout(orderedFields);
    updateTitlePositionVariable();
  }

  void TrackColumnController::updateTitlePositionVariable()
  {
    double titleX = 0;
    bool found = false;
    auto const titleFieldId = rt::trackFieldId(rt::TrackField::Title);

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

      if (col->get_id().raw() == titleFieldId)
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

  TrackColumnController::ColumnBinding* TrackColumnController::findColumnBinding(rt::TrackField field)
  {
    auto const it = std::ranges::find(_columns, field, &ColumnBinding::field);

    return it != _columns.end() ? &*it : nullptr;
  }

  TrackColumnController::ColumnBinding const* TrackColumnController::findColumnBinding(rt::TrackField field) const
  {
    auto const it = std::ranges::find(_columns, field, &ColumnBinding::field);

    return it != _columns.end() ? &*it : nullptr;
  }
} // namespace ao::gtk
