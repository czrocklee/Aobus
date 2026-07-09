// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
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
#include <cmath>
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

    _columnView.signal_map().connect(
      [this]
      {
        queueColumnResolve();
        queueTitlePositionVariableUpdate();
      });

    if (auto const adjPtr = _columnView.get_hadjustment(); adjPtr)
    {
      adjPtr->property_page_size().signal_changed().connect(
        [this]
        {
          queueColumnResolve();
          queueTitlePositionVariableUpdate();
        });
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

  void TrackColumnController::configureColumns(FactoryProvider const& factoryProvider)
  {
    auto const wasSyncingColumnLayout = _syncingColumnLayout;
    _syncingColumnLayout = true;

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
        [this, field = rtDef.field, columnPtr]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          _optPendingUserResize = PendingUserResize{.field = field, .width = columnPtr->get_fixed_width()};
          queueSharedColumnLayoutUpdate();
        }));

      _columnNotifyConnections.emplace_back(columnPtr->property_fixed_width().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::queueTitlePositionVariableUpdate)));

      _columnView.append_column(columnPtr);

      _columns.push_back({.field = rtDef.field, .columnPtr = columnPtr});
    }

    _syncingColumnLayout = wasSyncingColumnLayout;
  }

  void TrackColumnController::applyColumnLayout(std::span<rt::TrackField const> visibleFields)
  {
    auto const wasSyncingColumnLayout = _syncingColumnLayout;
    _syncingColumnLayout = true;

    if (auto const columnsPtr = _columnView.get_columns(); columnsPtr)
    {
      // Reorder columns to match visibleFields order
      for (auto const& [index, field] : std::views::enumerate(visibleFields))
      {
        auto* const binding = findColumnBinding(field);

        if (binding == nullptr)
        {
          continue;
        }

        ensureColumnPosition(columnsPtr, static_cast<std::size_t>(index), binding->columnPtr);
      }
    }

    updateColumnVisibility(visibleFields);

    auto const specs = uimodel::pixelTrackColumnSpecs(visibleFields, _layoutStore.layoutForList(_listId));
    applySolvedColumnWidths(specs);

    _syncingColumnLayout = wasSyncingColumnLayout;
    queueTitlePositionVariableUpdate();
  }

  void TrackColumnController::ensureColumnPosition(Glib::RefPtr<Gio::ListModel> const& columnsPtr,
                                                   std::size_t index,
                                                   Glib::RefPtr<Gtk::ColumnViewColumn> const& columnPtr)
  {
    bool needsMove = true;

    if (columnsPtr->get_n_items() > index)
    {
      if (auto currentObjPtr = columnsPtr->get_object(static_cast<::guint>(index)); currentObjPtr)
      {
        if (currentObjPtr == columnPtr)
        {
          needsMove = false;
        }
      }
    }

    if (needsMove)
    {
      for (::guint loopIndex = 0; loopIndex < columnsPtr->get_n_items(); ++loopIndex)
      {
        if (columnsPtr->get_object(loopIndex) == columnPtr)
        {
          _columnView.remove_column(columnPtr);
          break;
        }
      }

      _columnView.insert_column(static_cast<::guint>(index), columnPtr);
    }
  }

  void TrackColumnController::setLayoutAndApply(std::span<rt::TrackField const> visibleFields)
  {
    auto const orderedFields = visibleFieldsInStoredOrder(visibleFields);
    applyColumnLayout(orderedFields);
  }

  void TrackColumnController::queueSharedColumnLayoutUpdate()
  {
    if (_syncingColumnLayout || _queuedColumnLayoutUpdateConnection.connected())
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

  void TrackColumnController::queueColumnResolve()
  {
    if (_syncingColumnLayout || _queuedColumnResolveConnection.connected())
    {
      return;
    }

    _queuedColumnResolveConnection =
      Glib::signal_idle().connect(sigc::mem_fun(*this, &TrackColumnController::flushColumnResolve));
  }

  bool TrackColumnController::flushColumnResolve()
  {
    _queuedColumnResolveConnection.disconnect();

    if (_syncingColumnLayout)
    {
      return false;
    }

    auto const visibleFields = visibleFieldsInColumnOrder();
    auto const specs = uimodel::pixelTrackColumnSpecs(visibleFields, _layoutStore.layoutForList(_listId));
    applySolvedColumnWidths(specs);
    updateTitlePositionVariable();

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
    auto const visibleFields = visibleFieldsInColumnOrder();
    auto const priorSpecs = uimodel::pixelTrackColumnSpecs(visibleFields, _layoutStore.layoutForList(_listId));
    std::int32_t const viewportWidth = resolvedViewportWidth();
    auto specs = std::vector<uimodel::TrackColumnSolveSpec>{};

    if (_optPendingUserResize && viewportWidth > 0)
    {
      specs = uimodel::resizeTrackColumnSpecs(
        priorSpecs, _optPendingUserResize->field, _optPendingUserResize->width, viewportWidth);
      _optPendingUserResize.reset();
    }
    else
    {
      specs = uimodel::specsFromWidths(priorSpecs, visibleColumnWidths());
      _optPendingUserResize.reset();
    }

    auto layout = std::vector<uimodel::TrackColumnState>{};
    layout.reserve(specs.size());

    for (auto const& spec : specs)
    {
      layout.push_back(uimodel::canonicalTrackColumnState(spec));
    }

    _layoutStore.updateLayout(_listId, layout);

    if (viewportWidth > 0)
    {
      applySolvedColumnWidths(specs);
    }

    updateTitlePositionVariable();
  }

  void TrackColumnController::applySolvedColumnWidths(std::span<uimodel::TrackColumnSolveSpec const> specs)
  {
    auto const widths = uimodel::solveTrackColumnWidths(specs, resolvedViewportWidth());

    if (widths.size() != specs.size())
    {
      return;
    }

    auto const wasSyncingColumnLayout = _syncingColumnLayout;
    _syncingColumnLayout = true;

    for (std::size_t index = 0; index < specs.size(); ++index)
    {
      auto* const binding = findColumnBinding(specs[index].field);

      if (binding == nullptr)
      {
        continue;
      }

      if (binding->columnPtr->get_fixed_width() != widths[index])
      {
        binding->columnPtr->set_fixed_width(widths[index]);
      }
    }

    _syncingColumnLayout = wasSyncingColumnLayout;
  }

  std::int32_t TrackColumnController::resolvedViewportWidth() const
  {
    if (auto const adjPtr = _columnView.get_hadjustment(); adjPtr)
    {
      if (auto const pageSize = adjPtr->get_page_size(); pageSize > 0.0)
      {
        return static_cast<std::int32_t>(std::lround(pageSize));
      }
    }

    return std::max(0, _columnView.get_width());
  }

  std::vector<rt::TrackField> TrackColumnController::visibleFieldsInColumnOrder() const
  {
    auto fields = std::vector<rt::TrackField>{};
    auto const columnsPtr = _columnView.get_columns();

    if (!columnsPtr)
    {
      return fields;
    }

    fields.reserve(columnsPtr->get_n_items());

    for (std::uint32_t index = 0; index < columnsPtr->get_n_items(); ++index)
    {
      auto const gtkColumnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));

      if (!gtkColumnPtr || !gtkColumnPtr->get_visible())
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

  std::vector<std::int32_t> TrackColumnController::visibleColumnWidths() const
  {
    auto widths = std::vector<std::int32_t>{};
    auto const columnsPtr = _columnView.get_columns();

    if (!columnsPtr)
    {
      return widths;
    }

    widths.reserve(columnsPtr->get_n_items());

    for (std::uint32_t index = 0; index < columnsPtr->get_n_items(); ++index)
    {
      auto const gtkColumnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));

      if (gtkColumnPtr && gtkColumnPtr->get_visible())
      {
        widths.push_back(gtkColumnPtr->get_fixed_width());
      }
    }

    return widths;
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

    int const viewWidth = _columnView.get_width();

    double titleX = 0;
    bool found = false;
    auto const titleFieldId = rt::trackFieldId(rt::TrackField::Title);

    for (std::uint32_t index = 0; index < columnsPtr->get_n_items(); ++index)
    {
      auto const colPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));

      if (!colPtr || !colPtr->get_visible())
      {
        continue;
      }

      if (colPtr->get_id().raw() == titleFieldId)
      {
        found = true;
        break;
      }

      titleX += std::max(0, colPtr->get_fixed_width());
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

  bool TrackColumnController::isTitlePositionUpdateQueued() const noexcept
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
