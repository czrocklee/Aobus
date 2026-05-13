// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowController.h"
#include "app/MainWindow.h"
#include <ao/audio/Player.h>
#include <ao/utility/Log.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/NotificationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>
#ifdef ALSA_FOUND
#include <ao/audio/backend/AlsaProvider.h>
#endif
#ifdef PIPEWIRE_FOUND
#include <ao/audio/backend/PipeWireProvider.h>
#endif

#include <runtime/StateTypes.h>

#include <algorithm>
#include <limits>
#include <ranges>

namespace ao::gtk
{
  namespace
  {
    ao::ListId allTracksListId()
    {
      return ao::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

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
        if (auto const column = trackColumnFromId(id))
        {
          if (std::ranges::find(ordered, *column, &TrackColumnState::column) != ordered.end())
          {
            continue;
          }
          if (auto stateEntry = takeColumn(*column))
          {
            ordered.push_back(*stateEntry);
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
  }

  WindowController::WindowController(MainWindow& window,
                                     ao::rt::AppSession& session,
                                     std::shared_ptr<ao::rt::ConfigStore> configStore)
    : _window{window}, _session{session}, _configStore{std::move(configStore)}
  {
    // Initialize cover art cache
    int const coverArtCacheSize = 100;
    _coverArtCache = std::make_unique<CoverArtCache>(coverArtCacheSize);

    // Initialize TagEditController
    _tagEditController =
      std::make_unique<TagEditController>(window, _session, TagEditController::Callbacks{.onTagsMutated = [] {}});

    // Initialize list sidebar controller (must exist before TrackPageManager)
    _listSidebarController = std::make_unique<ListSidebarController>(
      window,
      _session,
      ListSidebarController::Callbacks{
        .onListSelected = [this](ao::ListId listId) { _session.workspace().navigateTo(listId); },
        .getListMembership = [this](ao::ListId listId) { return &_session.sources().sourceFor(listId); }});

    // Initialize track page manager
    _trackPageManager = std::make_unique<TrackPageManager>(_stack,
                                                           _trackColumnLayoutModel,
                                                           _session,
                                                           _playbackSequenceController.get(),
                                                           *_tagEditController,
                                                           *_listSidebarController);

    // Initialize import/export coordinator
    _importExportCoordinator = std::make_unique<ImportExportCoordinator>(
      window,
      _session,
      ImportExportCallbacks{.onOpenNewLibrary = [](std::filesystem::path const&) {},
                            .onLibraryDataMutated =
                              [this]
                            {
                              if (_trackRowCache)
                              {
                                _trackRowCache->clearCache();
                                _session.reloadAllTracks();
                                auto const txn = _session.musicLibrary().readTransaction();
                                rebuildListPages(txn);
                              }
                            },
                            .onTitleChanged = [this](std::string const& title) { _window.set_title(title); }});
  }

  WindowController::~WindowController()
  {
    _tracksMutatedSubscription.reset();
    _importProgressSubscription.reset();
    _importCompletedSubscription.reset();
    _listsMutatedSubscription.reset();
  }

  void WindowController::initializeSession()
  {
    _trackRowCache = std::make_unique<TrackRowCache>(_session.musicLibrary());

    _session.reloadAllTracks();

#ifdef PIPEWIRE_FOUND
    _session.addAudioProvider(std::make_unique<ao::audio::backend::PipeWireProvider>());
#endif
#ifdef ALSA_FOUND
    _session.addAudioProvider(std::make_unique<ao::audio::backend::AlsaProvider>());
#endif

    _playbackSequenceController = std::make_unique<PlaybackSequenceController>(_session, *_trackRowCache);
    _trackPageManager->setPlaybackSequenceController(*_playbackSequenceController);

    _importCompletedSubscription = _session.mutation().onImportCompleted(
      [this](auto)
      {
        if (_trackRowCache)
        {
          _trackRowCache->clearCache();
          _session.reloadAllTracks();
        }
      });

    _tracksMutatedSubscription = _session.mutation().onTracksMutated(
      [this](auto const& trackIds)
      {
        if (!_trackRowCache)
        {
          return;
        }
        for (auto const trackId : trackIds)
        {
          _trackRowCache->invalidate(trackId);
        }
        _session.sources().allTracks().notifyUpdated(trackIds);
      });

    _listsMutatedSubscription = _session.mutation().onListsMutated(
      [this](auto const&)
      {
        if (_trackRowCache)
        {
          auto const txn = _session.musicLibrary().readTransaction();
          rebuildListPages(txn);
        }
      });

    _tagEditController->setDataProvider(_trackRowCache.get());

    _session.notifications().post(ao::rt::NotificationSeverity::Info, "Aobus Ready");

    auto const txn = _session.musicLibrary().readTransaction();
    rebuildListPages(txn);

    _session.workspace().restoreSession();

    if (_session.workspace().layoutState().openViews.empty())
    {
      _session.workspace().navigateTo(allTracksListId());
    }

    saveSession();
  }

  void WindowController::saveSession()
  {
    auto ws = WindowState{};

    if (auto const width = _window.get_width(); width > 0)
    {
      ws.width = width;
    }

    if (auto const height = _window.get_height(); height > 0)
    {
      ws.height = height;
    }

    ws.maximized = _window.is_maximized();

    _configStore->save("window", ws);

    _configStore->save("track_view", trackViewStateFromLayout(_trackColumnLayoutModel.layout()));

    _session.workspace().saveSession();
  }

  void WindowController::loadSession()
  {
    auto ws = WindowState{};

    if (auto const res = _configStore->load("window", ws); !res && res.error().code != ao::Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load window config: {}", res.error().message);
    }

    _window.set_default_size(ws.width, ws.height);

    if (ws.maximized)
    {
      _window.maximize();
    }

    auto tvs = TrackViewState{};

    if (auto const res = _configStore->load("track_view", tvs); !res && res.error().code != ao::Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load track view config: {}", res.error().message);
    }

    _trackColumnLayoutModel.setLayout(trackColumnLayoutFromState(tvs));
  }

  void WindowController::rebuildListPages(ao::lmdb::ReadTransaction const& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _trackPageManager->rebuild(*_trackRowCache, txn);

    if (_listSidebarController)
    {
      _listSidebarController->rebuildTree(*_trackRowCache, txn);
    }
  }
} // namespace ao::gtk
