// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SessionPersistence.h"
#include <ao/utility/Log.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    std::string normalizeLibraryPath(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      auto const canonicalPath = std::filesystem::weakly_canonical(path, ec);
      return ec ? path.lexically_normal().string() : canonicalPath.string();
    }

    TrackColumnLayout trackColumnLayoutFromState(ao::app::TrackViewState const& state)
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
        auto const column = trackColumnFromId(id);

        if (!column)
        {
          continue;
        }

        auto const existing = std::ranges::find(ordered, *column, &TrackColumnState::column);

        if (existing != ordered.end())
        {
          continue;
        }

        if (auto stateEntry = takeColumn(*column))
        {
          ordered.push_back(*stateEntry);
        }
      }

      for (auto const& entry : layout.columns)
      {
        auto const existing = std::ranges::find(ordered, entry.column, &TrackColumnState::column);

        if (existing == ordered.end())
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

    ao::app::TrackViewState trackViewStateFromLayout(TrackColumnLayout const& layout)
    {
      auto normalized = normalizeTrackColumnLayout(layout);
      auto state = ao::app::TrackViewState{};

      for (auto const& entry : normalized.columns)
      {
        auto const columnId = std::string{trackColumnId(entry.column)};
        state.columnOrder.push_back(columnId);

        if (!entry.visible)
        {
          state.hiddenColumns.push_back(columnId);
        }

        auto const definitionIt =
          std::ranges::find(trackColumnDefinitions(), entry.column, &TrackColumnDefinition::column);

        if (definitionIt != trackColumnDefinitions().end() && entry.width != definitionIt->defaultWidth)
        {
          state.columnWidths.insert_or_assign(columnId, entry.width);
        }
      }

      return state;
    }
  }

  SessionPersistence::SessionPersistence()
  {
  }

  void SessionPersistence::loadUi(Gtk::Window& window,
                                  Gtk::Paned& paned,
                                  TrackColumnLayoutModel& trackColumnLayoutModel)
  {
    try
    {
      _appConfig = ao::app::AppConfig::load();
      trackColumnLayoutModel.setLayout(trackColumnLayoutFromState(_appConfig.trackViewState()));

      auto const& windowState = _appConfig.windowState();
      window.set_default_size(windowState.width, windowState.height);

      if (windowState.panedPosition > 0)
      {
        paned.set_position(windowState.panedPosition);
      }

      if (windowState.maximized)
      {
        window.maximize();
      }
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to load ui session: {}", e.what());
    }
  }

  void SessionPersistence::saveUi(Gtk::Window const& window,
                                  Gtk::Paned const& paned,
                                  TrackColumnLayoutModel const& trackColumnLayoutModel)
  {
    try
    {
      auto windowState = _appConfig.windowState();

      if (auto const width = window.get_width(); width > 0)
      {
        windowState.width = width;
      }

      if (auto const height = window.get_height(); height > 0)
      {
        windowState.height = height;
      }

      if (auto const panedPosition = paned.get_position(); panedPosition > 0)
      {
        windowState.panedPosition = panedPosition;
      }

      windowState.maximized = window.is_maximized();
      _appConfig.setWindowState(windowState);

      _appConfig.setTrackViewState(trackViewStateFromLayout(trackColumnLayoutModel.layout()));

      _appConfig.save();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save ui session: {}", e.what());
    }
  }

  std::optional<ao::app::SessionSnapshot> SessionPersistence::loadSnapshot()
  {
    try
    {
      _appConfig = ao::app::AppConfig::load();
      auto const& sessionState = _appConfig.sessionState();

      auto snapshot = ao::app::SessionSnapshot{};
      snapshot.lastBackend = sessionState.lastBackend;
      snapshot.lastProfile = sessionState.lastProfile;
      snapshot.lastOutputDeviceId = sessionState.lastOutputDeviceId;
      snapshot.lastLibraryPath = sessionState.lastLibraryPath;

      // Note: in a fully neutral shell, AppConfig would map view configs directly.
      // For now, GTK relies on its MainWindow loadSession to reconstruct views.
      // We will slowly move that logic here.

      return snapshot;
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to load session snapshot: {}", e.what());
      return std::nullopt;
    }
  }

  void SessionPersistence::saveSnapshot(ao::app::SessionSnapshot const& snapshot)
  {
    try
    {
      auto sessionState = _appConfig.sessionState();

      if (!snapshot.lastLibraryPath.empty())
      {
        sessionState.lastLibraryPath = normalizeLibraryPath(std::filesystem::path{snapshot.lastLibraryPath});
      }
      else
      {
        sessionState.lastLibraryPath = std::string{};
      }

      sessionState.lastBackend = snapshot.lastBackend;
      sessionState.lastProfile = snapshot.lastProfile;
      sessionState.lastOutputDeviceId = snapshot.lastOutputDeviceId;

      _appConfig.setSessionState(std::move(sessionState));
      _appConfig.save();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save session snapshot: {}", e.what());
    }
  }
} // namespace ao::gtk
