// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WindowStatePersistence.h"

#include "UIState.h"
#include "ao/utility/Log.h"
#include "runtime/ConfigStore.h"
#include "runtime/TrackField.h"
#include "track/TrackPresentation.h"

#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace ao::gtk
{
  namespace
  {
    TrackColumnViewState viewStateFromPersisted(TrackViewState const& state)
    {
      auto result = TrackColumnViewState{};

      for (auto const& [columnId, width] : state.columnWidths)
      {
        if (auto const optField = rt::trackFieldFromId(columnId))
        {
          result.widths.at(static_cast<std::size_t>(*optField)) = width;
        }
      }

      for (auto const& columnId : state.columnOrder)
      {
        if (auto const optField = rt::trackFieldFromId(columnId);
            optField && !std::ranges::contains(result.fieldOrder, *optField))
        {
          result.fieldOrder.push_back(*optField);
        }
      }

      return result;
    }

    TrackViewState viewStateToPersisted(TrackColumnViewState const& state)
    {
      auto result = TrackViewState{};

      for (auto const& def : rt::trackFieldDefinitions())
      {
        auto const width = state.widths.at(static_cast<std::size_t>(def.field));

        if (width != 0)
        {
          result.columnWidths.insert_or_assign(std::string{def.id}, width);
        }
      }

      for (auto const field : state.fieldOrder)
      {
        if (auto const* def = rt::trackFieldDefinition(field); def != nullptr)
        {
          result.columnOrder.emplace_back(def->id);
        }
      }

      return result;
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

    model.setState(viewStateFromPersisted(tvs));
  }

  void WindowStatePersistence::saveTrackView(TrackColumnLayoutModel const& model) const
  {
    _configStore.save("track_view", viewStateToPersisted(model.state()));
  }
} // namespace ao::gtk
