// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfigStore.h"
#include "app/GtkLayoutStateStore.h"
#include "app/GtkUiDependencies.h"
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
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
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

#include <gtkmm/stack.h>

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
  struct MainWindowCoordinator::Impl final
  {
    Impl(MainWindowCoordinator* coordinator, MainWindow& window, rt::AppRuntime& runtime)
      : layoutStateStore{runtime.musicLibrary().rootPath() / ".aobus"}
      , trackRowCache{runtime.library()}
      , imageCache{100}
      , playbackCommandSurface{runtime.playback(),
                               runtime.playbackSequence(),
                               [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }}
      , trackPresentationCatalog{runtime.workspace()}
      , trackPresentationPreferences{trackPresentationCatalog}
      , tagEditController{window, runtime, TagEditController::Callbacks{.onTagsMutated = [] {}}, themeCoordinator}
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
                                 themeCoordinator}
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
                                    coordinator->rebuildListPages();
                                  },
                                  .onTitleChanged = [&window](std::string const& title) { window.set_title(title); }},
                                themeCoordinator}
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
      for (auto const viewId : runtime.workspace().snapshot().openViews)
      {
        auto const state = runtime.views().trackListState(viewId);

        if (state.listId == kInvalidListId)
        {
          continue;
        }

        if (auto result = runtime.views().setPresentation(viewId, presentationForList(state.listId, runtime)); !result)
        {
          APP_LOG_ERROR("Failed to apply presentation preference: {}", result.error().message);
        }
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

    GtkLayoutStateStore layoutStateStore;
    ThemeCoordinator themeCoordinator;
    TrackRowCache trackRowCache;
    ImageCache imageCache;
    uimodel::PlaybackCommandSurface playbackCommandSurface;
    ao::uimodel::TrackPresentationCatalog trackPresentationCatalog;
    ao::uimodel::ListPresentationPreferenceStore trackPresentationPreferences;
    ao::uimodel::TrackColumnLayoutStore trackColumnLayouts;
    TagEditController tagEditController;
    ListNavigationController listNavigationController;
    Gtk::Stack stack;
    TrackPageHost trackPageHost;
    portal::ImportExportCoordinator importExportCoordinator;
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

    _tracksMutatedSubscription = _runtime.library().changes().onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        if (changeSet.libraryReset)
        {
          _implPtr->trackRowCache.clearCache();
          return;
        }

        auto trackIds = changeSet.tracksInserted;
        trackIds.append_range(changeSet.tracksDeleted);
        trackIds.append_range(changeSet.tracksMutated);

        for (auto const trackId : trackIds)
        {
          _implPtr->trackRowCache.invalidate(trackId);
        }
      });

    _listsMutatedSubscription = _runtime.library().changes().onChanged(
      [this](rt::LibraryChangeSet const& mutation)
      {
        if (!mutation.libraryReset && mutation.listsUpserted.empty() && mutation.listsDeleted.empty() &&
            mutation.manualContentChanges.empty())
        {
          return;
        }

        for (auto const deletedId : mutation.listsDeleted)
        {
          _implPtr->trackPresentationPreferences.clearPresentationForList(deletedId);
        }

        rebuildListPages();
      });

    _runtime.notifications().post(rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Info,
      .message = "Aobus Ready",
      .activityPresentation = rt::NotificationActivityPresentation::Hidden,
    });

    if (auto const restored = _runtime.workspace().restoreSession(_runtime.workspaceConfigStore()); !restored)
    {
      APP_LOG_WARN("MainWindowCoordinator: Failed to restore workspace session - {}", restored.error().message);
    }

    _implPtr->applyPresentationPreferencesToOpenViews(_runtime);

    if (_runtime.workspace().snapshot().openViews.empty())
    {
      auto const spec = _implPtr->presentationForList(rt::kAllTracksListId, _runtime);
      std::ignore = _runtime.workspace().navigateTo(rt::kAllTracksListId, {.optPresentation = spec});
      _runtime.workspace().saveSession(_runtime.workspaceConfigStore());
    }

    rebuildListPages();
    _implPtr->restorePlaybackSession(_runtime);
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

    if (auto const saved = _runtime.savePlaybackSession(); !saved)
    {
      APP_LOG_WARN("MainWindowCoordinator: Failed to checkpoint playback session - {}", saved.error().message);
    }

    _runtime.workspace().saveSession(_runtime.workspaceConfigStore());
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
    _implPtr->layoutStateStore.load(columnState, prefState);
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

    _implPtr->themeCoordinator.load(*_configStorePtr);
    _optThemeToken = _implPtr->themeCoordinator.registerToplevel(_window);
  }

  GtkUiDependencies MainWindowCoordinator::uiDependencies()
  {
    return GtkUiDependencies{
      .trackRowCache = &_implPtr->trackRowCache,
      .imageCache = &_implPtr->imageCache,
      .playbackSequence = &_runtime.playbackSequence(),
      .playbackCommandSurface = &_implPtr->playbackCommandSurface,
      .tagEditController = &_implPtr->tagEditController,
      .importExportActions = &_implPtr->importExportCoordinator,
      .trackPageHost = &_implPtr->trackPageHost,
      .trackPresentationCatalog = &_implPtr->trackPresentationCatalog,
      .trackPresentationPreferences = &_implPtr->trackPresentationPreferences,
      .listNavigationController = &_implPtr->listNavigationController,
      .themeCoordinator = &_implPtr->themeCoordinator,
      .createSmartListFromExpression = [navigationController = &_implPtr->listNavigationController](
                                         ao::ListId parentListId, std::string expression)
      { navigationController->createSmartListFromExpression(parentListId, std::move(expression)); }};
  }

  void MainWindowCoordinator::rebuildListPages()
  {
    APP_LOG_DEBUG("rebuildListPages called");
    _implPtr->trackPageHost.rebuild(_implPtr->trackRowCache);

    _implPtr->listNavigationController.rebuildTree(_implPtr->trackRowCache);
  }

  void MainWindowCoordinator::saveColumnLayout()
  {
    auto columnState = ao::uimodel::TrackColumnLayoutState{};
    columnState.listLayouts = _implPtr->trackColumnLayouts.listLayouts();

    auto prefState = ao::uimodel::ListPresentationPreferenceState{};
    prefState.presentations = _implPtr->trackPresentationPreferences.listPresentations();

    _implPtr->layoutStateStore.save(columnState, prefState);
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
  ThemeCoordinator* MainWindowCoordinator::themeCoordinator()
  {
    return &_implPtr->themeCoordinator;
  }
  portal::ImportExportCoordinator& MainWindowCoordinator::importExport()
  {
    return _implPtr->importExportCoordinator;
  }
} // namespace ao::gtk
