// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfig.h"
#include "app/GtkLayoutConfig.h"
#include "app/GtkUiServices.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "app/ThemePreset.h"
#include "app/UIState.h"
#include "image/ImageCache.h"
#include "list/ListNavigationController.h"
#include "platform/AudioBackendBootstrap.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinator.h"
#include "tag/TagEditController.h"
#include "track/TrackPageHost.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <gtkmm/stack.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  struct MainWindowCoordinator::Impl final
  {
    Impl(MainWindowCoordinator* coordinator, MainWindow& window, rt::AppRuntime& runtime)
      : layoutConfig{runtime.musicLibrary().rootPath() / ".aobus"}
      , trackRowCache{runtime.library()}
      , imageCache{100}
      , playbackQueueModel{runtime.playback()}
      , tagEditController{window, runtime, TagEditController::Callbacks{.onTagsMutated = [] {}}, themeController}
      , listNavigationController{window,
                                 runtime,
                                 ListNavigationController::Callbacks{
                                   .onListSelected =
                                     [&runtime, this](ListId listId)
                                   {
                                     auto const spec = presentationForList(listId, runtime);
                                     runtime.workspace().navigateTo(listId, {.optPresentation = spec});
                                   },
                                   .getListMembership = [&runtime](ListId listId)
                                   { return &runtime.sources().sourceFor(listId); },
                                   .onListPresentationSaved = [this](ListId listId, std::string const& presentationId)
                                   { trackPresentationStore.setPresentationIdForList(listId, presentationId); },
                                   .getListPresentation = [this](ListId listId) -> std::optional<std::string>
                                   {
                                     if (auto const optPres = trackPresentationStore.presentationIdForList(listId);
                                         optPres)
                                     {
                                       return std::string{*optPres};
                                     }

                                     return std::nullopt;
                                   }},
                                 themeController}
      , trackPresentationStore{runtime.workspace()}
      , trackPageHost{stack,
                      runtime,
                      &playbackQueueModel,
                      tagEditController,
                      listNavigationController,
                      trackPresentationStore}
      , importExportCoordinator{window,
                                runtime,
                                portal::ImportExportCallbacks{
                                  .onOpenNewLibrary = [](std::filesystem::path const&) {},
                                  .onLibraryDataMutated =
                                    [coordinator, &runtime, this]
                                  {
                                    trackRowCache.clearCache();
                                    runtime.reloadAllTracks();
                                    auto const txn = runtime.musicLibrary().readTransaction();
                                    coordinator->rebuildListPages(txn);
                                  },
                                  .onTitleChanged = [&window](std::string const& title) { window.set_title(title); }},
                                themeController}
    {
      tagEditController.setDataProvider(&trackRowCache);
    }

    std::string smartListFilter(ListId listId, rt::AppRuntime& runtime) const
    {
      auto filter = std::string{};

      if (listId == rt::kAllTracksListId || listId == kInvalidListId)
      {
        return filter;
      }

      auto const readTxn = runtime.musicLibrary().readTransaction();

      if (auto const optView = runtime.musicLibrary().lists().reader(readTxn).get(listId);
          optView && optView->isSmart())
      {
        filter = optView->filter();
      }

      return filter;
    }

    rt::TrackPresentationSpec presentationForList(ListId listId, rt::AppRuntime& runtime) const
    {
      return trackPresentationStore.presentationForList(listId, smartListFilter(listId, runtime));
    }

    void applyPresentationPreferencesToOpenViews(rt::AppRuntime& runtime) const
    {
      for (auto const viewId : runtime.workspace().layoutState().openViews)
      {
        auto const state = runtime.views().trackListState(viewId);

        if (state.listId == kInvalidListId)
        {
          continue;
        }

        runtime.views().setPresentation(viewId, presentationForList(state.listId, runtime));
      }
    }

    GtkLayoutConfig layoutConfig;
    ThemeCoordinator themeController;
    TrackRowCache trackRowCache;
    ImageCache imageCache;
    uimodel::playback::PlaybackQueueModel playbackQueueModel;
    TagEditController tagEditController;
    ListNavigationController listNavigationController;
    ao::uimodel::track::TrackPresentationViewModel trackPresentationStore;
    Gtk::Stack stack;
    TrackPageHost trackPageHost;
    portal::ImportExportCoordinator importExportCoordinator;
  };

  MainWindowCoordinator::MainWindowCoordinator(MainWindow& window,
                                               rt::AppRuntime& runtime,
                                               std::shared_ptr<AppConfig> configPtr)
    : _window{window}, _runtime{runtime}, _configPtr{std::move(configPtr)}
  {
    _implPtr = std::make_unique<Impl>(this, window, runtime);

    _trackPresentationChangedSubscription = _implPtr->trackPresentationStore.signalChanged().connect(
      [this](ao::ListId /*listId*/, ao::uimodel::track::TrackPresentationChangeType /*type*/) { saveColumnLayout(); });
  }

  MainWindowCoordinator::~MainWindowCoordinator()
  {
    _tracksMutatedSubscription.reset();
    _libraryTaskProgressSubscription.reset();
    _libraryTaskCompletedSubscription.reset();
    _listsMutatedSubscription.reset();
    _trackPresentationChangedSubscription.reset();
  }

  void MainWindowCoordinator::initializeSession()
  {
    _runtime.reloadAllTracks();

    registerPlatformAudioBackends(_runtime);

    _libraryTaskCompletedSubscription = _runtime.library().changes().onLibraryTaskCompleted(
      [this](auto)
      {
        _implPtr->trackRowCache.clearCache();
        _runtime.reloadAllTracks();
      });

    _tracksMutatedSubscription = _runtime.library().changes().onTracksMutated(
      [this](auto const& trackIds)
      {
        for (auto const trackId : trackIds)
        {
          _implPtr->trackRowCache.invalidate(trackId);
        }

        _runtime.sources().allTracks().notifyUpdated(trackIds);
      });

    _listsMutatedSubscription = _runtime.library().changes().onListsMutated(
      [this](auto const& mutation)
      {
        for (auto const deletedId : mutation.deleted)
        {
          _implPtr->trackPresentationStore.clearPresentationForList(deletedId);
        }

        auto const txn = _runtime.musicLibrary().readTransaction();
        rebuildListPages(txn);
      });

    _runtime.notifications().post(rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Info,
      .message = "Aobus Ready",
      .activityPresentation = rt::NotificationActivityPresentation::Hidden,
    });

    auto const txn = _runtime.musicLibrary().readTransaction();
    rebuildListPages(txn);

    _runtime.workspace().restoreSession(_runtime.configStore());
    _implPtr->applyPresentationPreferencesToOpenViews(_runtime);

    if (_runtime.workspace().layoutState().openViews.empty())
    {
      auto const spec = _implPtr->presentationForList(rt::kAllTracksListId, _runtime);
      _runtime.workspace().navigateTo(rt::kAllTracksListId, {.optPresentation = spec});
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
    _configPtr->saveWindow(windowState);

    saveColumnLayout();

    // App prefs (including playback state and last library)
    auto prefs = rt::AppPrefsState{};
    _configPtr->loadAppPrefs(prefs);

    prefs.lastLibraryPath = _runtime.musicLibrary().rootPath().string();
    auto const& pb = _runtime.playback().state();
    prefs.lastBackend = pb.selectedOutput.backendId.raw();
    prefs.lastOutputDeviceId = pb.selectedOutput.deviceId.raw();
    prefs.lastProfile = pb.selectedOutput.profileId.raw();
    prefs.lastThemePreset = std::string{themePresetToString(_implPtr->themeController.activeTheme())};

    _configPtr->saveAppPrefs(prefs);

    _runtime.workspace().saveSession(_runtime.configStore());
  }

  void MainWindowCoordinator::loadSession()
  {
    // Window state
    auto windowState = WindowState{};
    _configPtr->loadWindow(windowState);
    _window.set_default_size(windowState.width, windowState.height);

    if (windowState.maximized)
    {
      _window.maximize();
    }

    // Column layouts (widths and order)
    auto columnState = ao::uimodel::track::TrackColumnLayoutState{};
    auto prefState = ao::uimodel::track::ListPresentationPreferenceState{};
    _implPtr->layoutConfig.load(columnState, prefState);
    _implPtr->trackPresentationStore.setListLayouts(columnState.listLayouts);
    _implPtr->trackPresentationStore.setListPresentations(prefState.presentations);

    // App prefs (playback restoration)
    auto prefs = rt::AppPrefsState{};
    _configPtr->loadAppPrefs(prefs);

    if (!prefs.lastBackend.empty())
    {
      _runtime.playback().setOutput(audio::BackendId{prefs.lastBackend},
                                    audio::DeviceId{prefs.lastOutputDeviceId},
                                    audio::ProfileId{prefs.lastProfile});
    }

    _implPtr->themeController.load(*_configPtr);
    _optThemeToken = _implPtr->themeController.registerToplevel(_window);
  }

  GtkUiServices MainWindowCoordinator::uiServices()
  {
    return GtkUiServices{.trackRowCache = &_implPtr->trackRowCache,
                         .imageCache = &_implPtr->imageCache,
                         .playbackQueueModel = &_implPtr->playbackQueueModel,
                         .tagEditController = &_implPtr->tagEditController,
                         .importExportCoordinator = &_implPtr->importExportCoordinator,
                         .trackPageHost = &_implPtr->trackPageHost,
                         .trackPresentationStore = &_implPtr->trackPresentationStore,
                         .listNavigationController = &_implPtr->listNavigationController,
                         .themeController = &_implPtr->themeController};
  }

  void MainWindowCoordinator::rebuildListPages(lmdb::ReadTransaction const& txn)
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _implPtr->trackPageHost.rebuild(_implPtr->trackRowCache, txn);

    _implPtr->listNavigationController.rebuildTree(_implPtr->trackRowCache);
  }

  void MainWindowCoordinator::saveColumnLayout()
  {
    auto columnState = ao::uimodel::track::TrackColumnLayoutState{};
    columnState.listLayouts = _implPtr->trackPresentationStore.listLayouts();

    auto prefState = ao::uimodel::track::ListPresentationPreferenceState{};
    prefState.presentations = _implPtr->trackPresentationStore.listPresentations();

    _implPtr->layoutConfig.save(columnState, prefState);
  }

  TrackRowCache* MainWindowCoordinator::trackRowCache()
  {
    return &_implPtr->trackRowCache;
  }
  ImageCache* MainWindowCoordinator::imageCache()
  {
    return &_implPtr->imageCache;
  }
  uimodel::playback::PlaybackQueueModel* MainWindowCoordinator::playbackQueueModel()
  {
    return &_implPtr->playbackQueueModel;
  }
  TagEditController* MainWindowCoordinator::tagEditController()
  {
    return &_implPtr->tagEditController;
  }
  portal::ImportExportCoordinator* MainWindowCoordinator::importExportCoordinator()
  {
    return &_implPtr->importExportCoordinator;
  }
  TrackPageHost* MainWindowCoordinator::trackPageHost()
  {
    return &_implPtr->trackPageHost;
  }
  ListNavigationController* MainWindowCoordinator::listNavigationController()
  {
    return &_implPtr->listNavigationController;
  }
  uimodel::track::TrackPresentationViewModel* MainWindowCoordinator::trackPresentationStore()
  {
    return &_implPtr->trackPresentationStore;
  }
  ThemeCoordinator* MainWindowCoordinator::themeController()
  {
    return &_implPtr->themeController;
  }
  portal::ImportExportCoordinator& MainWindowCoordinator::importExport()
  {
    return _implPtr->importExportCoordinator;
  }
} // namespace ao::gtk
