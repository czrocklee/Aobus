// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "ao/Type.h"
#include "app/UIState.h"
#include "track/TrackFieldUi.h"
#include "track/TrackPresentationStore.h"
#include <ao/rt/TrackField.h>

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

  TrackColumnController::TrackColumnController(Gtk::ColumnView& columnView,
                                               TrackPresentationStore& presentationStore,
                                               ao::ListId listId)
    : _listId{listId}, _columnView{columnView}, _presentationStore{presentationStore}
  {
    _dynamicCssProvider = Gtk::CssProvider::create();

    _layoutChangedConnection = _presentationStore.signalChanged().connect(
      [this](ao::ListId listId, TrackPresentationChangeType type)
      {
        if (_capturingColumnLayout)
        {
          return;
        }

        if (listId != _listId && listId != ao::kInvalidListId)
        {
          return;
        }

        if (type == TrackPresentationChangeType::FullRebuild)
        {
          // FullRebuild might change field order/visibility handled by TrackViewPage,
          // but we still want to ensure our columns match the store's state if we're active.
          queueSharedColumnLayoutUpdate();
        }
      });

    _columnView.signal_map().connect(sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable));

    if (auto const adj = _columnView.get_hadjustment())
    {
      adj->property_page_size().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable));
      adj->property_upper().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable));
    }

    if (auto const columns = _columnView.get_columns())
    {
      _columnNotifyConnections.emplace_back(columns->signal_items_changed().connect(
        [this](::guint, ::guint, ::guint)
        {
          updateTitlePositionVariable();

          if (!_syncingColumnLayout)
          {
            queueSharedColumnLayoutUpdate();
          }
        }));
    }
  }

  TrackColumnController::~TrackColumnController() = default;

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

      column->set_id(Glib::ustring{rtDef.id.data(), rtDef.id.size()});

      column->set_resizable(true);

      column->set_fixed_width(defaultWidthForField(rtDef.field));

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

      _columns.push_back({.field = rtDef.field, .column = column, .defaultWidth = defaultWidthForField(rtDef.field)});
    }
  }

  namespace
  {
    rt::TrackField chooseExpandingColumn(std::span<rt::TrackField const> visibleFields)
    {
      if (std::ranges::contains(visibleFields, rt::TrackField::Tags))
      {
        return rt::TrackField::Tags;
      }

      if (std::ranges::contains(visibleFields, rt::TrackField::Title))
      {
        return rt::TrackField::Title;
      }

      if (!visibleFields.empty())
      {
        return visibleFields.front();
      }

      return rt::TrackField::Title;
    }
  } // namespace

  void TrackColumnController::applyColumnLayout(std::span<rt::TrackField const> visibleFields)
  {
    _syncingColumnLayout = true;
    _layoutChangedConnection.block();

    if (auto const columns = _columnView.get_columns(); columns)
    {
      auto const& storedLayout = _presentationStore.layoutForList(_listId);

      // Reorder columns to match visibleFields order
      for (auto const& [idx, field] : std::views::enumerate(visibleFields))
      {
        auto* const binding = findColumnBinding(field);

        if (binding == nullptr)
        {
          continue;
        }

        ensureColumnPosition(columns, idx, binding->column);

        // Find width in stored layout if it exists
        auto width = 0;
        auto const it = std::ranges::find(storedLayout, field, &ColumnState::field);

        if (it != storedLayout.end())
        {
          width = it->width;
        }

        auto const effectiveWidth = (width <= 0 ? binding->defaultWidth : width);

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
                                                   std::size_t index,
                                                   Glib::RefPtr<Gtk::ColumnViewColumn> const& column)
  {
    bool needsMove = true;

    if (columns->get_n_items() > index)
    {
      if (auto currentObj = columns->get_object(static_cast<::guint>(index)))
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

      _columnView.insert_column(static_cast<::guint>(index), column);
    }
  }

  void TrackColumnController::setLayoutAndApply(std::span<rt::TrackField const> visibleFields)
  {
    auto const orderedFields = visibleFieldsInStoredOrder(visibleFields);
    applyColumnLayout(orderedFields);
  }

  void TrackColumnController::updateColumnExpansion(std::span<rt::TrackField const> visibleFields)
  {
    auto const expanding = chooseExpandingColumn(visibleFields);

    for (auto& data : _columns)
    {
      data.column->set_expand(data.field == expanding);
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

    auto layout = std::vector<ColumnState>{};

    if (auto const columns = _columnView.get_columns(); columns)
    {
      for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
      {
        auto const obj = columns->get_object(idx);
        auto const gtkColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(obj);

        if (!gtkColumn || !gtkColumn->get_visible())
        {
          continue;
        }

        auto const optField = rt::trackFieldFromId(gtkColumn->get_id().raw());

        if (!optField)
        {
          continue;
        }

        layout.push_back({.field = *optField, .width = gtkColumn->get_fixed_width()});
      }
    }

    _presentationStore.updateLayout(_listId, layout);
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

    for (auto const field : _presentationStore.activeFieldOrder())
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
    auto const columns = _columnView.get_columns();

    if (!columns)
    {
      return;
    }

    std::int32_t totalFixedWidth = 0;

    for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
    {
      auto const col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(idx));

      if (col && col->get_visible())
      {
        totalFixedWidth += std::max(0, col->get_fixed_width());
      }
    }

    int const viewWidth = _columnView.get_width();
    int const extraSpace = std::max(0, viewWidth - totalFixedWidth);

    double titleX = 0;
    bool found = false;
    auto const titleFieldId = rt::trackFieldId(rt::TrackField::Title);

    for (std::uint32_t idx = 0; idx < columns->get_n_items(); ++idx)
    {
      auto const col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(idx));

      if (!col || !col->get_visible())
      {
        continue;
      }

      int const actualWidth = std::max(0, col->get_fixed_width()) + (col->get_expand() ? extraSpace : 0);

      if (col->get_id().raw() == titleFieldId)
      {
        found = true;
        break;
      }

      titleX += actualWidth;
    }

    if (found)
    {
      auto css = std::string{};

      if (viewWidth > 0)
      {
        double const percentage = (titleX / viewWidth) * 100.0;
        css = std::format("columnview {{ --ao-title-x: {:.1f}%; }}", percentage);
      }
      else
      {
        css = std::format("columnview {{ --ao-title-x: {:.1f}px; }}", titleX);
      }

      if (css != _lastTitleCss)
      {
        _lastTitleCss = css;
        _dynamicCssProvider->load_from_data(_lastTitleCss);
      }
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
