// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "ao/Type.h"
#include "ao/lmdb/Transaction.h"
#include "ao/utility/Log.h"
#include "app/GtkUiServices.h"
#include "app/MainWindow.h"
#include "app/WindowStatePersistence.h"
#include "inspector/CoverArtCache.h"
#include "list/ListSidebarController.h"
#include "platform/AudioBackendBootstrap.h"
#include "playback/PlaybackSequenceController.h"
#include "portal/ImportExportCoordinator.h"
#include "runtime/AppRuntime.h"
#include "runtime/CorePrimitives.h"
#include "runtime/LibraryMutationService.h"
#include "runtime/ListSourceStore.h"
#include "runtime/NotificationService.h"
#include "runtime/PlaybackService.h"
#include "runtime/SessionPersistenceService.h"
#include "runtime/StateTypes.h"
#include "runtime/ViewService.h"
#include "runtime/WorkspaceService.h"
#include "tag/TagEditController.h"
#include "track/TrackPageHost.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowCache.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  MainWindowCoordinator::MainWindowCoordinator(MainWindow& window,
                                               rt::AppRuntime& runtime,
                                               std::shared_ptr<rt::ConfigStore> globalConfig,
                                               std::shared_ptr<rt::ConfigStore> workspaceConfig)
    : _window{window}
    , _runtime{runtime}
    , _globalConfig{std::move(globalConfig)}
    , _workspaceConfig{std::move(workspaceConfig)}
  {
    _persistence = std::make_unique<WindowStatePersistence>(*_globalConfig);

    // Initialize cover art cache
    int const coverArtCacheSize = 100;
    _coverArtCache = std::make_unique<CoverArtCache>(coverArtCacheSize);

    // Initialize TagEditController
    _tagEditController =
      std::make_unique<TagEditController>(window, _runtime, TagEditController::Callbacks{.onTagsMutated = [] {}});

    // Initialize list sidebar controller (must exist before TrackPageHost)
    _listSidebarController = std::make_unique<ListSidebarController>(
      window,
      _runtime,
      ListSidebarController::Callbacks{
        .onListSelected = [this](ListId listId) { _runtime.workspace().navigateTo(listId); },
        .getListMembership = [this](ListId listId) { return &_runtime.sources().sourceFor(listId); }});

    // Initialize track presentation store
    _trackPresentationStore = std::make_unique<TrackPresentationStore>(_workspaceConfig);

    // Initialize track page manager
    _trackPageHost = std::make_unique<TrackPageHost>(_stack,
                                                     _trackColumnLayoutModel,
                                                     _runtime,
                                                     _playbackSequenceController.get(),
                                                     *_tagEditController,
                                                     *_listSidebarController,
                                                     *_trackPresentationStore);

    // Initialize import/export coordinator
    _importExportCoordinator = std::make_unique<portal::ImportExportCoordinator>(
      window,
      _runtime,
      portal::ImportExportCallbacks{.onOpenNewLibrary = [](std::filesystem::path const&) {},
                                    .onLibraryDataMutated =
                                      [this]
                                    {
                                      if (_trackRowCache)
                                      {
                                        _trackRowCache->clearCache();
                                        _runtime.reloadAllTracks();
                                        auto const txn = _runtime.musicLibrary().readTransaction();
                                        rebuildListPages(txn);
                                      }
                                    },
                                    .onTitleChanged = [this](std::string const& title) { _window.set_title(title); }});
  }

  MainWindowCoordinator::~MainWindowCoordinator()
  {
    _tracksMutatedSubscription.reset();
    _importProgressSubscription.reset();
    _importCompletedSubscription.reset();
    _listsMutatedSubscription.reset();
  }

  void MainWindowCoordinator::initializeSession()
  {
    _trackRowCache = std::make_unique<TrackRowCache>(_runtime.musicLibrary());

    _runtime.reloadAllTracks();

    registerPlatformAudioBackends(_runtime);

    _playbackSequenceController = std::make_unique<PlaybackSequenceController>(_runtime.playback(), *_trackRowCache);
    _trackPageHost->setPlaybackSequenceController(*_playbackSequenceController);

    _importCompletedSubscription = _runtime.mutation().onImportCompleted(
      [this](auto)
      {
        if (_trackRowCache)
        {
          _trackRowCache->clearCache();
          _runtime.reloadAllTracks();
        }
      });

    _tracksMutatedSubscription = _runtime.mutation().onTracksMutated(
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

        _runtime.sources().allTracks().notifyUpdated(trackIds);
      });

    _listsMutatedSubscription = _runtime.mutation().onListsMutated(
      [this](auto const&)
      {
        if (_trackRowCache)
        {
          auto const txn = _runtime.musicLibrary().readTransaction();
          rebuildListPages(txn);
        }
      });

    _tagEditController->setDataProvider(_trackRowCache.get());

    _runtime.notifications().post(rt::NotificationSeverity::Info, "Aobus Ready");

    auto const txn = _runtime.musicLibrary().readTransaction();
    rebuildListPages(txn);

    _runtime.persistence().restore();

    if (_runtime.workspace().layoutState().openViews.empty())
    {
      _runtime.workspace().navigateTo(rt::kAllTracksListId);
    }

    saveSession();
  }

  void MainWindowCoordinator::saveSession()
  {
    _persistence->saveWindow(_window);
    _persistence->saveTrackView(_trackColumnLayoutModel);

    _runtime.persistence().save();
  }

  void MainWindowCoordinator::loadSession()
  {
    _persistence->loadWindow(_window);
    _persistence->loadTrackView(_trackColumnLayoutModel);
  }

  GtkUiServices MainWindowCoordinator::uiServices()
  {
    return GtkUiServices{.trackRowCache = _trackRowCache.get(),
                         .coverArtCache = _coverArtCache.get(),
                         .playbackSequenceController = _playbackSequenceController.get(),
                         .tagEditController = _tagEditController.get(),
                         .importExportCoordinator = _importExportCoordinator.get(),
                         .trackPageHost = _trackPageHost.get(),
                         .columnLayoutModel = &_trackColumnLayoutModel,
                         .listSidebarController = _listSidebarController.get()};
  }

  void MainWindowCoordinator::rebuildListPages(lmdb::ReadTransaction const& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _trackPageHost->rebuild(*_trackRowCache, txn);

    if (_listSidebarController)
    {
      _listSidebarController->rebuildTree(*_trackRowCache, txn);
    }
  }
} // namespace ao::gtk
