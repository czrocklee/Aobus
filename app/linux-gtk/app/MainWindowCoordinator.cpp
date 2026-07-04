// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfig.h"
#include "app/GtkLayoutConfig.h"
#include "app/GtkUiServices.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "app/UIState.h"
#include "image/ImageCache.h"
#include "list/ListNavigationController.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinator.h"
#include "tag/TagEditController.h"
#include "track/TrackPageHost.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Transport.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <glibmm/main.h>
#include <gtkmm/stack.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kPlaybackSessionAutosaveInterval = std::chrono::seconds{10};
  } // namespace

  struct MainWindowCoordinator::Impl final
  {
    Impl(MainWindowCoordinator* coordinator, MainWindow& window, rt::AppRuntime& runtime)
      : layoutConfig{runtime.musicLibrary().rootPath() / ".aobus"}
      , trackRowCache{runtime.library()}
      , imageCache{100}
      , playbackQueueModel{runtime.playback(), runtime.notifications()}
      , trackPresentationCatalog{runtime.workspace()}
      , trackPresentationPreferences{trackPresentationCatalog}
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
                                   { trackPresentationPreferences.setPresentationIdForList(listId, presentationId); },
                                   .getListPresentation = [this](ListId listId) -> std::optional<std::string>
                                   {
                                     if (auto const optPres =
                                           trackPresentationPreferences.presentationIdForList(listId);
                                         optPres)
                                     {
                                       return std::string{*optPres};
                                     }

                                     return std::nullopt;
                                   }},
                                 themeController}
      , trackPageHost{stack,
                      runtime,
                      &playbackQueueModel,
                      tagEditController,
                      listNavigationController,
                      trackColumnLayouts}
      , importExportCoordinator{window,
                                runtime,
                                portal::ImportExportCallbacks{
                                  .onOpenNewLibrary = [](std::filesystem::path const&, bool) {},
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
      return trackPresentationPreferences.presentationForList(listId, smartListFilter(listId, runtime));
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

    std::vector<TrackId> trackIdsFrom(rt::TrackSource const& source) const
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(source.size());

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        trackIds.push_back(source.trackIdAt(index));
      }

      return trackIds;
    }

    void restorePlaybackSession(rt::AppRuntime& runtime)
    {
      auto session = rt::PlaybackSessionState{};

      if (auto const res = runtime.configStore().load(rt::kPlaybackSessionConfigGroup, session); !res)
      {
        if (res.error().code != Error::Code::NotFound)
        {
          APP_LOG_WARN("MainWindowCoordinator: Failed to restore playback session - {}", res.error().message);
        }

        return;
      }

      if (session.trackId == kInvalidTrackId)
      {
        return;
      }

      // ListSourceStore::sourceFor silently falls back to the all-tracks source
      // when a list id no longer exists. Resolve the saved list id explicitly so
      // the queue receives the list id we actually want restored, instead of a
      // fallback we inferred from a pointer comparison.
      auto const sourceListId = resolveRestoreSourceListId(runtime, session.sourceListId);

      if (auto& source = runtime.sources().sourceFor(sourceListId);
          !playbackQueueModel.restoreQueue(trackIdsFrom(source), session, sourceListId))
      {
        APP_LOG_DEBUG("MainWindowCoordinator: Playback session restore skipped");
        return;
      }

      if (auto const optRestoredTrackId = playbackQueueModel.nowPlayingTrackId(); optRestoredTrackId)
      {
        auto const spec = presentationForList(sourceListId, runtime);
        runtime.workspace().navigateTo(sourceListId, {.optPresentation = spec});
        runtime.playback().revealTrack(*optRestoredTrackId, rt::kInvalidViewId, sourceListId);
      }
    }

    ListId resolveRestoreSourceListId(rt::AppRuntime& runtime, ListId savedListId) const
    {
      if (savedListId == kInvalidListId || savedListId == rt::kAllTracksListId)
      {
        return rt::kAllTracksListId;
      }

      auto const txn = runtime.musicLibrary().readTransaction();
      auto const optView = rt::storageValueOrNullopt(
        runtime.musicLibrary().lists().reader(txn).get(savedListId), "Restored playback session list lookup");

      if (!optView)
      {
        APP_LOG_DEBUG(
          "MainWindowCoordinator: Saved source list {} no longer exists, restoring from All Tracks", savedListId.raw());
        return rt::kAllTracksListId;
      }

      return savedListId;
    }

    void savePlaybackSession(rt::AppRuntime& runtime)
    {
      if (runtime.playback().sessionState().trackId == kInvalidTrackId)
      {
        return;
      }

      runtime.playback().saveSession(runtime.configStore());
    }

    void startPlaybackSessionPersistence(rt::AppRuntime& runtime)
    {
      playbackPausedSub.reset();
      playbackStoppedSub.reset();
      playbackNowPlayingSub.reset();
      playbackSeekSub.reset();
      playbackSessionAutosaveConn.disconnect();

      playbackPausedSub = runtime.playback().onPaused([this, &runtime] { savePlaybackSession(runtime); });
      playbackStoppedSub = runtime.playback().onStopped([this, &runtime] { savePlaybackSession(runtime); });
      playbackNowPlayingSub = runtime.playback().onNowPlayingChanged(
        [this, &runtime](rt::PlaybackService::NowPlayingChanged const&) { savePlaybackSession(runtime); });
      playbackSeekSub = runtime.playback().onSeekUpdate(
        [this, &runtime](rt::PlaybackService::SeekUpdate const& event)
        {
          if (event.mode == rt::PlaybackService::SeekMode::Final)
          {
            savePlaybackSession(runtime);
          }
        });

      playbackSessionAutosaveConn = Glib::signal_timeout().connect(
        [this, &runtime]
        {
          if (runtime.playback().state().transport == audio::Transport::Playing)
          {
            savePlaybackSession(runtime);
          }

          return true;
        },
        std::chrono::duration_cast<std::chrono::milliseconds>(kPlaybackSessionAutosaveInterval).count());
    }

    GtkLayoutConfig layoutConfig;
    ThemeCoordinator themeController;
    TrackRowCache trackRowCache;
    ImageCache imageCache;
    uimodel::PlaybackQueueModel playbackQueueModel;
    ao::uimodel::TrackPresentationCatalog trackPresentationCatalog;
    ao::uimodel::ListPresentationPreferenceStore trackPresentationPreferences;
    ao::uimodel::TrackColumnLayoutStore trackColumnLayouts;
    TagEditController tagEditController;
    ListNavigationController listNavigationController;
    Gtk::Stack stack;
    TrackPageHost trackPageHost;
    portal::ImportExportCoordinator importExportCoordinator;
    rt::Subscription playbackPausedSub;
    rt::Subscription playbackStoppedSub;
    rt::Subscription playbackNowPlayingSub;
    rt::Subscription playbackSeekSub;
    sigc::scoped_connection playbackSessionAutosaveConn;
  };

  MainWindowCoordinator::MainWindowCoordinator(MainWindow& window,
                                               rt::AppRuntime& runtime,
                                               std::shared_ptr<AppConfig> configPtr)
    : _window{window}, _runtime{runtime}, _configPtr{std::move(configPtr)}
  {
    _implPtr = std::make_unique<Impl>(this, window, runtime);

    _trackPresentationChangedSubscription = _implPtr->trackPresentationPreferences.signalChanged().connect(
      [this](ao::ListId /*listId*/) { saveColumnLayout(); });
    _trackColumnLayoutChangedSubscription =
      _implPtr->trackColumnLayouts.signalChanged().connect([this](ao::ListId /*listId*/) { saveColumnLayout(); });
  }

  MainWindowCoordinator::~MainWindowCoordinator()
  {
    _tracksMutatedSubscription.reset();
    _libraryTaskCompletedSubscription.reset();
    _listsMutatedSubscription.reset();
    _trackPresentationChangedSubscription.reset();
    _trackColumnLayoutChangedSubscription.reset();
  }

  void MainWindowCoordinator::initializeSession()
  {
    _runtime.reloadAllTracks();

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
          _implPtr->trackPresentationPreferences.clearPresentationForList(deletedId);
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

    _implPtr->restorePlaybackSession(_runtime);
    _implPtr->startPlaybackSessionPersistence(_runtime);
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

    // Session state: per-window shutdown must not overwrite explicit application preferences.
    auto session = rt::AppSessionState{};
    _configPtr->loadAppSession(session);

    session.lastLibraryPath = _runtime.musicLibrary().rootPath().string();
    auto const& pb = _runtime.playback().state();
    session.lastOutputBackendId = pb.selectedOutputDevice.backendId.raw();
    session.lastOutputDeviceId = pb.selectedOutputDevice.deviceId.raw();
    session.lastOutputProfileId = pb.selectedOutputDevice.profileId.raw();

    _configPtr->saveAppSession(session);

    _implPtr->savePlaybackSession(_runtime);
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
    auto columnState = ao::uimodel::TrackColumnLayoutState{};
    auto prefState = ao::uimodel::ListPresentationPreferenceState{};
    _implPtr->layoutConfig.load(columnState, prefState);
    _implPtr->trackColumnLayouts.setListLayouts(columnState.listLayouts);
    _implPtr->trackPresentationPreferences.setListPresentations(prefState.presentations);

    // App prefs (playback restoration)
    auto prefs = rt::AppPrefsState{};
    _configPtr->loadAppPrefs(prefs);
    auto session = rt::AppSessionState{};
    _configPtr->loadAppSession(session);

    bool const hasPreferredOutput = !prefs.lastOutputBackendId.empty() && !prefs.lastOutputProfileId.empty();
    auto const& outputBackendId = hasPreferredOutput ? prefs.lastOutputBackendId : session.lastOutputBackendId;
    auto const& outputDeviceId = hasPreferredOutput ? prefs.lastOutputDeviceId : session.lastOutputDeviceId;
    auto const& outputProfileId = hasPreferredOutput ? prefs.lastOutputProfileId : session.lastOutputProfileId;

    if (!outputBackendId.empty())
    {
      _runtime.playback().setOutputDevice(
        audio::BackendId{outputBackendId}, audio::DeviceId{outputDeviceId}, audio::ProfileId{outputProfileId});
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
                         .trackPresentationCatalog = &_implPtr->trackPresentationCatalog,
                         .trackPresentationPreferences = &_implPtr->trackPresentationPreferences,
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
    auto columnState = ao::uimodel::TrackColumnLayoutState{};
    columnState.listLayouts = _implPtr->trackColumnLayouts.listLayouts();

    auto prefState = ao::uimodel::ListPresentationPreferenceState{};
    prefState.presentations = _implPtr->trackPresentationPreferences.listPresentations();

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
  uimodel::PlaybackQueueModel* MainWindowCoordinator::playbackQueueModel()
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
  uimodel::TrackPresentationCatalog* MainWindowCoordinator::trackPresentationCatalog()
  {
    return &_implPtr->trackPresentationCatalog;
  }
  uimodel::ListPresentationPreferenceStore* MainWindowCoordinator::trackPresentationPreferences()
  {
    return &_implPtr->trackPresentationPreferences;
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
