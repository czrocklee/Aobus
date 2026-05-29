// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfig.h"
#include "app/GtkLayoutConfig.h"
#include "app/GtkUiServices.h"
#include "app/MainWindow.h"
#include "app/UIState.h"
#include "image/ImageCache.h"
#include "list/ListNavigationController.h"
#include "platform/AudioBackendBootstrap.h"
#include "portal/ImportExportCoordinator.h"
#include "tag/TagEditController.h"
#include "track/TrackPageHost.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/utility/Log.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  MainWindowCoordinator::MainWindowCoordinator(MainWindow& window,
                                               rt::AppRuntime& runtime,
                                               std::shared_ptr<AppConfig> config)
    : _window{window}, _runtime{runtime}, _config{std::move(config)}
  {
    _layoutConfig = std::make_unique<GtkLayoutConfig>(_runtime.musicLibrary().rootPath() / ".aobus");

    // Initialize cover art cache
    int const imageCacheSize = 100;
    _imageCache = std::make_unique<ImageCache>(imageCacheSize);

    // Initialize TagEditController
    _tagEditController =
      std::make_unique<TagEditController>(window, _runtime, TagEditController::Callbacks{.onTagsMutated = [] {}});

    // Initialize list sidebar controller (must exist before TrackPageHost)
    _listSidebarController = std::make_unique<ListNavigationController>(
      window,
      _runtime,
      ListNavigationController::Callbacks{
        .onListSelected = [this](ListId listId) { _runtime.workspace().navigateTo(listId); },
        .getListMembership = [this](ListId listId) { return &_runtime.sources().sourceFor(listId); }});

    // Initialize track presentation store
    _trackPresentationStore = std::make_unique<TrackPresentationStore>(_runtime.workspace());
    _trackPresentationChangedConnection = _trackPresentationStore->signalChanged().connect(
      [this](ao::ListId /*listId*/, TrackPresentationChangeType type)
      {
        if (type == TrackPresentationChangeType::LayoutOnly)
        {
          saveColumnLayout();
        }
      });

    // Initialize track page manager
    _trackPageHost = std::make_unique<TrackPageHost>(_stack,
                                                     _runtime,
                                                     _playbackQueueModel.get(),
                                                     *_tagEditController,
                                                     *_listSidebarController,
                                                     *_trackPresentationStore,
                                                     _imageCache.get());

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
    _libraryTaskProgressSubscription.reset();
    _libraryTaskCompletedSubscription.reset();
    _listsMutatedSubscription.reset();
  }

  void MainWindowCoordinator::initializeSession()
  {
    _trackRowCache = std::make_unique<TrackRowCache>(_runtime.musicLibrary());

    _runtime.reloadAllTracks();

    registerPlatformAudioBackends(_runtime);

    _playbackQueueModel = std::make_unique<ao::uimodel::playback::PlaybackQueueModel>(
      _runtime.playback(), [this](TrackId id) { return _trackRowCache->playbackDescriptor(id); });
    _trackPageHost->setPlaybackQueueModel(*_playbackQueueModel);

    _libraryTaskCompletedSubscription = _runtime.mutation().onLibraryTaskCompleted(
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

    _runtime.workspace().restoreSession(_runtime.configStore());

    if (_runtime.workspace().layoutState().openViews.empty())
    {
      _runtime.workspace().navigateTo(rt::kAllTracksListId);
      _runtime.workspace().saveSession(_runtime.configStore());
    }
  }

  void MainWindowCoordinator::saveSession()
  {
    // Window state
    auto windowState = WindowState{};

    if (auto const width = _window.get_width(); width > 0)
    {
      windowState.width = width;
    }

    if (auto const height = _window.get_height(); height > 0)
    {
      windowState.height = height;
    }

    windowState.maximized = _window.is_maximized();
    _config->saveWindow(windowState);

    saveColumnLayout();

    // App prefs (including playback state and last library)
    auto prefs = rt::AppPrefsState{};
    prefs.lastLibraryPath = _runtime.musicLibrary().rootPath().string();
    auto const& pb = _runtime.playback().state();
    prefs.lastBackend = pb.selectedOutput.backendId.raw();
    prefs.lastOutputDeviceId = pb.selectedOutput.deviceId.raw();
    prefs.lastProfile = pb.selectedOutput.profileId.raw();
    _config->saveAppPrefs(prefs);

    _runtime.workspace().saveSession(_runtime.configStore());
  }

  void MainWindowCoordinator::loadSession()
  {
    // Window state
    auto windowState = WindowState{};
    _config->loadWindow(windowState);
    _window.set_default_size(windowState.width, windowState.height);

    if (windowState.maximized)
    {
      _window.maximize();
    }

    // Column layouts (widths and order)
    auto columnState = ColumnLayoutState{};
    _layoutConfig->load(columnState);
    _trackPresentationStore->setListLayouts(columnState.listLayouts);

    // App prefs (playback restoration)
    auto prefs = rt::AppPrefsState{};
    _config->loadAppPrefs(prefs);

    if (!prefs.lastBackend.empty())
    {
      _runtime.playback().setOutput(audio::BackendId{prefs.lastBackend},
                                    audio::DeviceId{prefs.lastOutputDeviceId},
                                    audio::ProfileId{prefs.lastProfile});
    }
  }

  GtkUiServices MainWindowCoordinator::uiServices()
  {
    return GtkUiServices{.trackRowCache = _trackRowCache.get(),
                         .imageCache = _imageCache.get(),
                         .playbackQueueModel = _playbackQueueModel.get(),
                         .tagEditController = _tagEditController.get(),
                         .importExportCoordinator = _importExportCoordinator.get(),
                         .trackPageHost = _trackPageHost.get(),
                         .trackPresentationStore = _trackPresentationStore.get(),
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

  void MainWindowCoordinator::saveColumnLayout()
  {
    auto columnState = ColumnLayoutState{};
    columnState.listLayouts = _trackPresentationStore->listLayouts();
    _layoutConfig->save(columnState);
  }
} // namespace ao::gtk
