// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include <ao/Type.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

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
  TrackColumnController::TrackColumnController(Gtk::ColumnView& columnView,
                                               uimodel::TrackColumnLayoutStore& layoutStore,
                                               ao::ListId listId)
    : _listId{listId}, _columnView{columnView}, _layoutStore{layoutStore}
  {
    _dynamicCssProviderPtr = Gtk::CssProvider::create();

    _layoutChangedSubscription = _layoutStore.signalChanged().connect(
      [this](ao::ListId listId)
      {
        if (_capturingColumnLayout || _syncingColumnLayout)
        {
          return;
        }

        if (listId != _listId && listId != ao::kInvalidListId)
        {
          return;
        }

        queueSharedColumnLayoutUpdate();
      });

    _columnView.signal_map().connect(sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable));

    if (auto const adjPtr = _columnView.get_hadjustment(); adjPtr)
    {
      adjPtr->property_page_size().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::queueTitlePositionVariableUpdate));
      adjPtr->property_upper().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::queueTitlePositionVariableUpdate));
    }

    if (auto const columnsPtr = _columnView.get_columns(); columnsPtr)
    {
      _columnNotifyConnections.emplace_back(columnsPtr->signal_items_changed().connect(
        [this](::guint, ::guint, ::guint)
        {
          queueTitlePositionVariableUpdate();

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
      auto const columnPtr = Gtk::ColumnViewColumn::create(title, factoryProvider(rtDef.field));

      columnPtr->set_id(Glib::ustring{rtDef.id.data(), rtDef.id.size()});

      columnPtr->set_resizable(true);

      columnPtr->set_fixed_width(uimodel::defaultTrackFieldColumnWidth(rtDef.field));

      _columnNotifyConnections.emplace_back(columnPtr->property_fixed_width().signal_changed().connect(
        [this]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        }));

      _columnNotifyConnections.emplace_back(columnPtr->property_fixed_width().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::queueTitlePositionVariableUpdate)));

      _columnView.append_column(columnPtr);

      _columns.push_back({.field = rtDef.field, .columnPtr = columnPtr});
    }
  }

  void TrackColumnController::applyColumnLayout(std::span<rt::TrackField const> visibleFields)
  {
    _syncingColumnLayout = true;

    if (auto const columnsPtr = _columnView.get_columns(); columnsPtr)
    {
      auto const& storedLayout = _layoutStore.layoutForList(_listId);

      // Reorder columns to match visibleFields order
      for (auto const& [idx, field] : std::views::enumerate(visibleFields))
      {
        auto* const binding = findColumnBinding(field);

        if (binding == nullptr)
        {
          continue;
        }

        ensureColumnPosition(columnsPtr, static_cast<std::size_t>(idx), binding->columnPtr);

        // Find width in stored layout if it exists
        std::int32_t width = 0;
        auto const it = std::ranges::find(storedLayout, field, &uimodel::TrackColumnState::field);

        if (it != storedLayout.end())
        {
          width = it->width;
        }

        auto const effectiveWidth = uimodel::effectiveTrackFieldColumnWidth(field, width);

        if (binding->columnPtr->get_fixed_width() != effectiveWidth)
        {
          binding->columnPtr->set_fixed_width(effectiveWidth);
        }
      }
    }

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
      if (auto currentObjPtr = columns->get_object(static_cast<::guint>(index)); currentObjPtr)
      {
        if (currentObjPtr == column)
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
    auto const expanding = uimodel::expandingTrackColumn(visibleFields);

    for (auto& data : _columns)
    {
      data.columnPtr->set_expand(data.field == expanding);
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

  void TrackColumnController::queueTitlePositionVariableUpdate()
  {
    if (_queuedTitlePositionUpdateConnection.connected())
    {
      return;
    }

    _queuedTitlePositionUpdateConnection =
      Glib::signal_idle().connect(sigc::mem_fun(*this, &TrackColumnController::flushTitlePositionVariableUpdate));
  }

  bool TrackColumnController::flushTitlePositionVariableUpdate()
  {
    _queuedTitlePositionUpdateConnection.disconnect();
    updateTitlePositionVariable();
    return false;
  }

  void TrackColumnController::updateSharedColumnLayout()
  {
    _capturingColumnLayout = true;

    auto layout = std::vector<uimodel::TrackColumnState>{};

    if (auto const columnsPtr = _columnView.get_columns(); columnsPtr)
    {
      for (std::uint32_t idx = 0; idx < columnsPtr->get_n_items(); ++idx)
      {
        auto const objPtr = columnsPtr->get_object(idx);
        auto const gtkColumnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(objPtr);

        if (!gtkColumnPtr || !gtkColumnPtr->get_visible())
        {
          continue;
        }

        auto const optField = rt::trackFieldFromId(gtkColumnPtr->get_id().raw());

        if (!optField)
        {
          continue;
        }

        layout.push_back({.field = *optField, .width = gtkColumnPtr->get_fixed_width()});
      }
    }

    _layoutStore.updateLayout(_listId, layout);
    _capturingColumnLayout = false;
  }

  std::vector<rt::TrackField> TrackColumnController::captureCurrentFieldOrder() const
  {
    auto fields = std::vector<rt::TrackField>{};
    auto const columnsPtr = _columnView.get_columns();

    if (!columnsPtr)
    {
      return fields;
    }

    fields.reserve(columnsPtr->get_n_items());

    for (std::uint32_t idx = 0; idx < columnsPtr->get_n_items(); ++idx)
    {
      auto const objPtr = columnsPtr->get_object(idx);
      auto const gtkColumnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(objPtr);

      if (!gtkColumnPtr)
      {
        continue;
      }

      if (auto const optField = rt::trackFieldFromId(std::string{gtkColumnPtr->get_id()}); optField)
      {
        fields.push_back(*optField);
      }
    }

    return fields;
  }

  std::vector<rt::TrackField> TrackColumnController::visibleFieldsInStoredOrder(
    std::span<rt::TrackField const> visibleFields) const
  {
    auto const activeOrder = _layoutStore.activeFieldOrder();
    return uimodel::visibleTrackFieldsInStoredOrder(visibleFields, activeOrder);
  }

  void TrackColumnController::updateColumnVisibility(std::span<rt::TrackField const> visibleFields)
  {
    for (auto& binding : _columns)
    {
      auto const visible = std::ranges::contains(visibleFields, binding.field);
      binding.columnPtr->set_visible(visible);
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
    auto const columnsPtr = _columnView.get_columns();

    if (!columnsPtr)
    {
      return;
    }

    std::int32_t totalFixedWidth = 0;

    for (std::uint32_t idx = 0; idx < columnsPtr->get_n_items(); ++idx)
    {
      auto const colPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(idx));

      if (colPtr && colPtr->get_visible())
      {
        totalFixedWidth += std::max(0, colPtr->get_fixed_width());
      }
    }

    int const viewWidth = _columnView.get_width();
    int const extraSpace = std::max(0, viewWidth - totalFixedWidth);

    double titleX = 0;
    bool found = false;
    auto const titleFieldId = rt::trackFieldId(rt::TrackField::Title);

    for (std::uint32_t idx = 0; idx < columnsPtr->get_n_items(); ++idx)
    {
      auto const colPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(idx));

      if (!colPtr || !colPtr->get_visible())
      {
        continue;
      }

      int const actualWidth = std::max(0, colPtr->get_fixed_width()) + (colPtr->get_expand() ? extraSpace : 0);

      if (colPtr->get_id().raw() == titleFieldId)
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
        _dynamicCssProviderPtr->load_from_data(_lastTitleCss);
      }
    }
  }

  bool TrackColumnController::isTitlePositionUpdateQueuedForTest() const noexcept
  {
    return _queuedTitlePositionUpdateConnection.connected();
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
