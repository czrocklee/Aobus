// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfigStore.h"
#include "app/GtkLayoutConfig.h"
#include "app/GtkUiServices.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "app/WindowState.h"
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
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackSessionSaveService.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <glibmm/main.h>
#include <gtkmm/stack.h>
#include <sigc++/connection.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kPlaybackSessionAutosaveInterval = std::chrono::seconds{10};

    class GlibPlaybackSessionSaveScheduler final : public rt::PlaybackSessionSaveService::Scheduler
    {
    public:
      rt::Subscription schedule(rt::PlaybackSessionSaveService::Delay const delay,
                                rt::PlaybackSessionSaveService::Callback callback) override
      {
        auto callbackPtr = std::make_shared<rt::PlaybackSessionSaveService::Callback>(std::move(callback));
        auto connectionPtr = std::make_shared<sigc::connection>();
        *connectionPtr = Glib::signal_timeout().connect(
          [callbackPtr]
          {
            auto callback = std::move(*callbackPtr);
            callback();
            return false;
          },
          static_cast<std::uint32_t>(delay.count()));
        return rt::Subscription{[connectionPtr] { connectionPtr->disconnect(); }};
      }
    };
  } // namespace

  struct MainWindowCoordinator::Impl final
  {
    Impl(MainWindowCoordinator* coordinator, MainWindow& window, rt::AppRuntime& runtime)
      : layoutConfig{runtime.musicLibrary().rootPath() / ".aobus"}
      , trackRowCache{runtime.library()}
      , imageCache{100}
      , playbackSessionSaveService{rt::PlaybackSessionSaveService::Port{
                                     .subscribeDirty = [&runtime](rt::PlaybackSessionSaveService::Callback callback)
                                     { return runtime.onPlaybackSessionDirty(std::move(callback)); },
                                     .save = [&runtime] { return runtime.savePlaybackSession(); },
                                   },
                                   playbackSessionSaveScheduler}
      , playbackCommandSurface{runtime.playback(),
                               runtime.playbackSequence(),
                               [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }}
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
                                     std::ignore = runtime.workspace().navigateTo(listId, {.optPresentation = spec});
                                   },
                                   .onListPresentationSaved = [this](ListId listId, std::string const& presentationId)
                                   { trackPresentationPreferences.setPresentationIdForList(listId, presentationId); },
                                   .listPresentationCallback = [this](ListId listId) -> std::optional<std::string>
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
      , trackPageHost{stack, runtime, tagEditController, listNavigationController, trackColumnLayouts}
      , importExportCoordinator{window,
                                runtime,
                                portal::ImportExportCallbacks{
                                  .onOpenNewLibrary = [](std::filesystem::path const&, bool) {},
                                  .onLibraryDataMutated =
                                    [coordinator, &runtime, this]
                                  {
                                    trackRowCache.clearCache();
                                    runtime.reloadAllTracks();
                                    auto const transaction = runtime.musicLibrary().readTransaction();
                                    coordinator->rebuildListPages(transaction);
                                  },
                                  .onTitleChanged = [&window](std::string const& title) { window.set_title(title); }},
                                themeController}
    {
      tagEditController.setDataProvider(&trackRowCache);
    }

    rt::TrackPresentationSpec presentationForList(ListId listId, rt::AppRuntime& runtime) const
    {
      if (listId == rt::kAllTracksListId || listId == kInvalidListId)
      {
        return trackPresentationPreferences.presentationForList(uimodel::ListPresentationContext{
          .listId = listId,
          .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
        });
      }

      auto const readTransaction = runtime.musicLibrary().readTransaction();
      auto const optView = runtime.musicLibrary().lists().reader(readTransaction).get(listId);

      if (!optView)
      {
        return trackPresentationPreferences.presentationForList(uimodel::ListPresentationContext{
          .listId = listId,
          .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
        });
      }

      return trackPresentationPreferences.presentationForList(uimodel::ListPresentationContext{
        .listId = listId,
        .sourceKind =
          optView->isSmart() ? uimodel::ListPresentationSourceKind::Smart : uimodel::ListPresentationSourceKind::Manual,
        .smartListFilter = optView->isSmart() ? optView->filter() : std::string_view{},
      });
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

    void restorePlaybackSession(rt::AppRuntime& runtime) const
    {
      auto restored = runtime.restorePlaybackSession();

      if (!restored)
      {
        APP_LOG_WARN("MainWindowCoordinator: Failed to restore playback session - {}", restored.error().message);
        return;
      }

      if (!restored->restored)
      {
        return;
      }

      auto const spec = presentationForList(restored->sourceListId, runtime);
      std::ignore = runtime.workspace().navigateTo(restored->sourceListId, {.optPresentation = spec});
      runtime.playback().revealTrack(restored->trackId, rt::kInvalidViewId, restored->sourceListId);
    }

    void bindPlaybackSessionSaveTriggers(rt::AppRuntime& runtime)
    {
      playbackPausedSub.reset();
      playbackStoppedSub.reset();
      playbackNowPlayingSub.reset();
      playbackSeekSub.reset();
      playbackSessionAutosaveConn.disconnect();

      playbackPausedSub = runtime.playback().onPaused([this] { playbackSessionSaveService.saveSignificantEvent(); });
      playbackStoppedSub = runtime.playback().onStopped([this] { playbackSessionSaveService.saveSignificantEvent(); });
      playbackNowPlayingSub = runtime.playback().onNowPlayingChanged(
        [this](rt::PlaybackService::NowPlayingChanged const&) { playbackSessionSaveService.saveSignificantEvent(); });
      playbackSeekSub = runtime.playback().onSeekUpdate(
        [this](rt::PlaybackService::SeekUpdate const& event)
        {
          if (event.mode == rt::PlaybackService::SeekMode::Final)
          {
            playbackSessionSaveService.saveSignificantEvent();
          }
        });

      playbackSessionAutosaveConn = Glib::signal_timeout().connect(
        [this, &runtime]
        {
          if (runtime.playback().state().transport == audio::Transport::Playing)
          {
            playbackSessionSaveService.savePeriodic();
          }

          return true;
        },
        std::chrono::duration_cast<std::chrono::milliseconds>(kPlaybackSessionAutosaveInterval).count());
    }

    Result<> shutdownPlaybackSessionPersistence()
    {
      playbackPausedSub.reset();
      playbackStoppedSub.reset();
      playbackNowPlayingSub.reset();
      playbackSeekSub.reset();
      playbackSessionAutosaveConn.disconnect();
      return playbackSessionSaveService.shutdown();
    }

    GtkLayoutConfig layoutConfig;
    ThemeCoordinator themeController;
    TrackRowCache trackRowCache;
    ImageCache imageCache;
    GlibPlaybackSessionSaveScheduler playbackSessionSaveScheduler;
    rt::PlaybackSessionSaveService playbackSessionSaveService;
    uimodel::PlaybackCommandSurface playbackCommandSurface;
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
                                               std::shared_ptr<AppConfigStore> configStorePtr)
    : _window{window}, _runtime{runtime}, _configStorePtr{std::move(configStorePtr)}
  {
    _implPtr = std::make_unique<Impl>(this, window, runtime);

    _trackPresentationChangedSubscription = _implPtr->trackPresentationPreferences.signalChanged().connect(
      [this](ao::ListId /*listId*/) { saveColumnLayout(); });
    _trackColumnLayoutChangedSubscription =
      _implPtr->trackColumnLayouts.signalChanged().connect([this](ao::ListId /*listId*/) { saveColumnLayout(); });
  }

  MainWindowCoordinator::~MainWindowCoordinator()
  {
    try
    {
      if (auto const saved = _implPtr->shutdownPlaybackSessionPersistence(); !saved)
      {
        APP_LOG_WARN(
          "MainWindowCoordinator: Failed to save playback session during shutdown - {}", saved.error().message);
      }
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- destruction must contain save and logging failures
    {
    }

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
      });

    _listsMutatedSubscription = _runtime.library().changes().onListsMutated(
      [this](auto const& mutation)
      {
        for (auto const deletedId : mutation.deleted)
        {
          _implPtr->trackPresentationPreferences.clearPresentationForList(deletedId);
        }

        auto const transaction = _runtime.musicLibrary().readTransaction();
        rebuildListPages(transaction);
      });

    _runtime.notifications().post(rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Info,
      .message = "Aobus Ready",
      .activityPresentation = rt::NotificationActivityPresentation::Hidden,
    });

    auto const transaction = _runtime.musicLibrary().readTransaction();
    rebuildListPages(transaction);

    if (auto const restored = _runtime.workspace().restoreSession(_runtime.configStore()); !restored)
    {
      APP_LOG_WARN("MainWindowCoordinator: Failed to restore workspace session - {}", restored.error().message);
    }

    _implPtr->applyPresentationPreferencesToOpenViews(_runtime);

    if (_runtime.workspace().layoutState().openViews.empty())
    {
      auto const spec = _implPtr->presentationForList(rt::kAllTracksListId, _runtime);
      std::ignore = _runtime.workspace().navigateTo(rt::kAllTracksListId, {.optPresentation = spec});
      _runtime.workspace().saveSession(_runtime.configStore());
    }

    _implPtr->playbackSessionSaveService.start();
    _implPtr->restorePlaybackSession(_runtime);
    _implPtr->bindPlaybackSessionSaveTriggers(_runtime);
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
    _configStorePtr->saveWindow(windowState);

    saveColumnLayout();

    // Session state: per-window shutdown must not overwrite explicit application preferences.
    auto session = rt::AppSessionState{};
    _configStorePtr->loadAppSession(session);

    session.lastLibraryPath = _runtime.musicLibrary().rootPath().string();
    auto const& pb = _runtime.playback().state();
    session.lastOutputBackendId = pb.output.selectedDevice.backendId.raw();
    session.lastOutputDeviceId = pb.output.selectedDevice.deviceId.raw();
    session.lastOutputProfileId = pb.output.selectedDevice.profileId.raw();

    _configStorePtr->saveAppSession(session);

    _implPtr->playbackSessionSaveService.saveSignificantEvent();
    _runtime.workspace().saveSession(_runtime.configStore());
  }

  void MainWindowCoordinator::loadSession()
  {
    // Window state
    auto windowState = WindowState{};
    _configStorePtr->loadWindow(windowState);
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
    _configStorePtr->loadAppPrefs(prefs);
    auto session = rt::AppSessionState{};
    _configStorePtr->loadAppSession(session);

    bool const hasPreferredOutput = !prefs.lastOutputBackendId.empty() && !prefs.lastOutputProfileId.empty();
    auto const& outputBackendId = hasPreferredOutput ? prefs.lastOutputBackendId : session.lastOutputBackendId;
    auto const& outputDeviceId = hasPreferredOutput ? prefs.lastOutputDeviceId : session.lastOutputDeviceId;
    auto const& outputProfileId = hasPreferredOutput ? prefs.lastOutputProfileId : session.lastOutputProfileId;

    if (!outputBackendId.empty())
    {
      _runtime.playback().setOutputDevice(
        audio::BackendId{outputBackendId}, audio::DeviceId{outputDeviceId}, audio::ProfileId{outputProfileId});
    }

    _implPtr->themeController.load(*_configStorePtr);
    _optThemeToken = _implPtr->themeController.registerToplevel(_window);
  }

  GtkUiServices MainWindowCoordinator::uiServices()
  {
    return GtkUiServices{.trackRowCache = &_implPtr->trackRowCache,
                         .imageCache = &_implPtr->imageCache,
                         .playbackSequence = &_runtime.playbackSequence(),
                         .playbackCommandSurface = &_implPtr->playbackCommandSurface,
                         .tagEditController = &_implPtr->tagEditController,
                         .importExportCoordinator = &_implPtr->importExportCoordinator,
                         .trackPageHost = &_implPtr->trackPageHost,
                         .trackPresentationCatalog = &_implPtr->trackPresentationCatalog,
                         .trackPresentationPreferences = &_implPtr->trackPresentationPreferences,
                         .listNavigationController = &_implPtr->listNavigationController,
                         .themeController = &_implPtr->themeController};
  }

  void MainWindowCoordinator::rebuildListPages(lmdb::ReadTransaction const& transaction)
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _implPtr->trackPageHost.rebuild(_implPtr->trackRowCache, transaction);

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
  rt::PlaybackSequenceService* MainWindowCoordinator::playbackSequence()
  {
    return &_runtime.playbackSequence();
  }
  uimodel::PlaybackCommandSurface* MainWindowCoordinator::playbackCommandSurface()
  {
    return &_implPtr->playbackCommandSurface;
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
