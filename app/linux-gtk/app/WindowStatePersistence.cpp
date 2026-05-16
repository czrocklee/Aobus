// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WindowStatePersistence.h"
#include "UIState.h"
#include "track/TrackPresentation.h"
#include <ao/utility/Log.h>
#include <runtime/ConfigStore.h>

#include <gtkmm/window.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    TrackColumnLayout trackColumnLayoutFromState(TrackViewState const& state)
    {
      auto layout = defaultTrackColumnLayout();
      auto ordered = std::vector<TrackColumnState>{};
      ordered.reserve(layout.columns.size());

      auto takeColumn = [&layout](TrackColumn column) -> std::optional<TrackColumnState>
      {
        auto const it = std::ranges::find(layout.columns, column, &TrackColumnState::column);

        if (it == layout.columns.end())
        {
          return std::nullopt;
        }

        return *it;
      };

      for (auto const& id : state.columnOrder)
      {
        if (auto const optColumn = trackColumnFromId(id))
        {
          if (std::ranges::find(ordered, *optColumn, &TrackColumnState::column) != ordered.end())
          {
            continue;
          }

          if (auto optStateEntry = takeColumn(*optColumn))
          {
            ordered.push_back(*optStateEntry);
          }
        }
      }

      for (auto const& entry : layout.columns)
      {
        if (std::ranges::find(ordered, entry.column, &TrackColumnState::column) == ordered.end())
        {
          ordered.push_back(entry);
        }
      }

      layout.columns = std::move(ordered);

      for (auto& entry : layout.columns)
      {
        auto const columnId = std::string{trackColumnId(entry.column)};

        if (std::ranges::contains(state.hiddenColumns, columnId))
        {
          entry.visible = false;
        }

        if (auto const width = state.columnWidths.find(columnId); width != state.columnWidths.end())
        {
          entry.width = width->second;
        }
      }

      return normalizeTrackColumnLayout(layout);
    }

    TrackViewState trackViewStateFromLayout(TrackColumnLayout const& layout)
    {
      auto const normalized = normalizeTrackColumnLayout(layout);
      auto state = TrackViewState{};

      for (auto const& entry : normalized.columns)
      {
        auto const columnId = std::string{trackColumnId(entry.column)};
        state.columnOrder.push_back(columnId);

        if (!entry.visible)
        {
          state.hiddenColumns.push_back(columnId);
        }

        if (auto const def = std::ranges::find(trackColumnDefinitions(), entry.column, &TrackColumnDefinition::column);
            def != trackColumnDefinitions().end() && entry.width != def->defaultWidth)
        {
          state.columnWidths.insert_or_assign(columnId, entry.width);
        }
      }

      return state;
    }
  } // namespace

  WindowStatePersistence::WindowStatePersistence(rt::ConfigStore& configStore)
    : _configStore{configStore}
  {
  }

  void WindowStatePersistence::loadWindow(Gtk::Window& window) const
  {
    auto ws = WindowState{};

    if (auto const res = _configStore.load("window", ws); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load window config: {}", res.error().message);
    }

    window.set_default_size(ws.width, ws.height);

    if (ws.maximized)
    {
      window.maximize();
    }
  }

  void WindowStatePersistence::saveWindow(Gtk::Window const& window) const
  {
    auto ws = WindowState{};

    if (auto const width = window.get_width(); width > 0)
    {
      ws.width = width;
    }

    if (auto const height = window.get_height(); height > 0)
    {
      ws.height = height;
    }

    ws.maximized = window.is_maximized();

    _configStore.save("window", ws);
  }

  void WindowStatePersistence::loadTrackView(TrackColumnLayoutModel& model) const
  {
    auto tvs = TrackViewState{};

    if (auto const res = _configStore.load("track_view", tvs); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load track_view config: {}", res.error().message);
    }

    model.setLayout(trackColumnLayoutFromState(tvs));
  }

  void WindowStatePersistence::saveTrackView(TrackColumnLayoutModel const& model) const
  {
    _configStore.save("track_view", trackViewStateFromLayout(model.layout()));
  }
} // namespace ao::gtk
