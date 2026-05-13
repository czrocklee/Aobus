// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"
#include "layout/LayoutConstants.h"
#include <gtkmm/checkbutton.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>

#include <format>
#include <ranges>
#include <unordered_set>

namespace ao::gtk
{
  namespace
  {
    constexpr double kTitlePositionDivisor = 2.0;
  }

  TrackColumnController::TrackColumnController(Gtk::ColumnView& columnView, TrackColumnLayoutModel& layoutModel)
    : _columnView{columnView}
    , _columnLayoutModel{layoutModel}
  {
    _dynamicCssProvider = Gtk::CssProvider::create();
  }

  void TrackColumnController::setupColumns(FactoryProvider factoryProvider)
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

      column->property_fixed_width().signal_changed().connect(
        [this]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        });

      _columnNotifyConnections.push_back(column->property_fixed_width().signal_changed().connect(
        sigc::mem_fun(*this, &TrackColumnController::updateTitlePositionVariable)));

      _columnView.append_column(column);

      auto* const toggle = Gtk::make_managed<Gtk::CheckButton>(title);

      toggle->signal_toggled().connect(
        [this, columnId = definition.column, toggleButton = toggle]
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          auto layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

          for (auto& state : layout.columns)
          {
            if (state.column == columnId)
            {
              state.visible = toggleButton->get_active();
              break;
            }
          }

          _columnLayoutModel.setLayout(layout);
        });

      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();

      row->set_child(*toggle);
      row->set_activatable(false);
      _columnToggleList.append(*row);

      auto binding = ColumnBinding{
        .id = definition.column, .column = column, .toggle = toggle, .defaultWidth = definition.defaultWidth};

      _columns.push_back(std::move(binding));
    }
  }

  void TrackColumnController::setupColumnControls()
  {
    _columnsButton.set_label("Columns");
    _columnsButton.set_popover(_columnsPopover);

    _columnsPopoverBox.set_spacing(Layout::kSpacingSmall);

    _columnsPopoverTitle.set_markup("<span size='small' weight='bold'>VISIBLE COLUMNS</span>");
    _columnsPopoverTitle.set_halign(Gtk::Align::START);
    _columnsPopoverTitle.add_css_class("dim-label");

    _columnToggleList.set_selection_mode(Gtk::SelectionMode::NONE);
    _columnToggleList.add_css_class("navigation-sidebar");

    _resetColumnsButton.set_label("Reset to Default");
    _resetColumnsButton.set_sensitive(true);
    _resetColumnsButton.add_css_class("suggested-action");
    _resetColumnsButton.signal_clicked().connect([this] { _columnLayoutModel.reset(); });

    _columnsPopoverBox.append(_columnsPopoverTitle);
    _columnsPopoverBox.append(_columnToggleList);
    _columnsPopoverBox.append(_columnsPopoverSeparator);
    _columnsPopoverBox.append(_resetColumnsButton);

    _columnsPopover.set_child(_columnsPopoverBox);
  }

  void TrackColumnController::applyColumnLayout()
  {
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    _syncingColumnLayout = true;

    for (auto const& [idx, state] : std::views::enumerate(layout.columns))
    {
      auto* const binding = findColumnBinding(state.column);

      if (binding == nullptr)
      {
        continue;
      }

      bool needsInsertion = true;

      if (_columnModel && _columnModel->get_n_items() > idx)
      {
        auto const object = _columnModel->get_object(static_cast<::guint>(idx));

        if (auto const currentColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);
            currentColumn && currentColumn->get_id() == binding->column->get_id())
        {
          needsInsertion = false;
        }
      }

      if (needsInsertion)
      {
        _columnView.insert_column(static_cast<::guint>(idx), binding->column);
      }

      auto const width = state.width == -1 ? binding->defaultWidth : state.width;

      if (binding->column->get_fixed_width() != width)
      {
        binding->column->set_fixed_width(width);
      }
    }

    syncColumnToggleStates();
    updateColumnVisibility();

    _syncingColumnLayout = false;
  }

  void TrackColumnController::syncColumnToggleStates()
  {
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    for (auto const& state : layout.columns)
    {
      auto* const binding = findColumnBinding(state.column);

      if (binding == nullptr || binding->toggle == nullptr)
      {
        continue;
      }

      binding->toggle->set_active(state.visible);
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

    if (!_columnModel)
    {
      return currentLayout;
    }

    auto const nItems = _columnModel->get_n_items();

    layout.columns.reserve(nItems);

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      auto const object = _columnModel->get_object(i);
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
    if (_columnVisibilityIdle)
    {
      return;
    }

    _columnVisibilityIdle = Glib::signal_idle().connect(
      [this]
      {
        _columnVisibilityIdle.disconnect();

        auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

        auto redundantColumns = std::unordered_set<TrackColumn>{};
        if (_redundancyProvider)
        {
          redundantColumns = _redundancyProvider();
        }

        for (auto const& state : layout.columns)
        {
          auto* const binding = findColumnBinding(state.column);

          if (binding == nullptr)
          {
            continue;
          }

          bool const hiddenByGroup = redundantColumns.contains(state.column);
          binding->column->set_visible(state.visible && !hiddenByGroup);
        }

        return false;
      });
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
