// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppRuntime.h>

#include "runtime/PlaybackSessionPersistence.h"
#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h> // NOLINT(misc-include-cleaner): unique_ptr<Executor> destruction needs the complete type.
#include <ao/async/Runtime.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Player.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <expected>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackTransport playbackTransport;
    PlaybackSuccession playbackSuccession;
    PlaybackBootstrap playbackBootstrap;
    std::unique_ptr<PlaybackService> playbackPtr;
    WorkspaceService workspaceService;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr;
    ConfigStore* playbackSessionConfigStore = nullptr;
    std::unique_ptr<PlaybackSessionPersistence> playbackSessionPersistencePtr;
    bool stopped = false;

    Impl(AppRuntime& runtime,
         std::unique_ptr<ConfigStore> workspaceConfigPtr,
         ConfigStore* playbackSessionConfigStoreValue)
      : viewService{runtime.async().callbackExecutor(),
                    runtime.musicLibrary(),
                    runtime.sources(),
                    runtime.library().changes()}
      , playbackTransport{runtime.async().callbackExecutor(),
                          runtime.musicLibrary(),
                          runtime.notifications(),
                          std::make_unique<audio::Player>(runtime.async())}
      , playbackSuccession{runtime.async().callbackExecutor(),
                           viewService,
                           runtime.sources(),
                           runtime.musicLibrary(),
                           playbackTransport,
                           runtime.notifications(),
                           runtime.async()}
      , playbackBootstrap{playbackTransport}
      , playbackPtr{playbackBootstrap.createPlaybackService(runtime.async().callbackExecutor(), playbackSuccession)}
      , workspaceService{runtime.async().callbackExecutor(), viewService, runtime.library().changes()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
      , playbackSessionConfigStore{playbackSessionConfigStoreValue != nullptr ? playbackSessionConfigStoreValue
                                                                              : workspaceConfigStorePtr.get()}
      , playbackSessionPersistencePtr{std::make_unique<PlaybackSessionPersistence>(*playbackSessionConfigStore,
                                                                                   runtime.library(),
                                                                                   playbackSuccession,
                                                                                   playbackTransport,
                                                                                   *playbackPtr,
                                                                                   runtime.async())}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { shutdown(); }

    void shutdown() noexcept
    {
      if (stopped)
      {
        return;
      }

      stopped = true;
      std::ignore = playbackSessionPersistencePtr->shutdown();
      // Join playback callback producers while every consumer is still alive.
      playbackPtr->shutdown();
      playbackBootstrap.shutdown();
    }
  };

  Result<std::unique_ptr<AppRuntime>> AppRuntime::create(AppRuntimeDependencies dependencies)
  {
    if (dependencies.executorPtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "AppRuntime requires an executor");
    }

    if (dependencies.workspaceConfigStorePtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "AppRuntime requires a workspace config store");
    }

    auto runtimePtr = std::unique_ptr<AppRuntime>{new AppRuntime{}};
    auto initializeRes = runtimePtr->initialize(std::move(dependencies.executorPtr),
                                                std::move(dependencies.musicRoot),
                                                std::move(dependencies.databasePath),
                                                std::move(dependencies.cacheDirectory),
                                                dependencies.musicLibraryPinnedMapBytes,
                                                dependencies.sleeper);

    if (!initializeRes)
    {
      return std::unexpected{initializeRes.error()};
    }

    runtimePtr->_implPtr = std::make_unique<Impl>(
      *runtimePtr, std::move(dependencies.workspaceConfigStorePtr), dependencies.playbackSessionConfigStore);
    return runtimePtr;
  }

  AppRuntime::AppRuntime() = default;
  AppRuntime::~AppRuntime() = default;

  void AppRuntime::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }

    CoreRuntime::shutdown();
  }

  PlaybackService& AppRuntime::playback() noexcept
  {
    return *_implPtr->playbackPtr;
  }

  WorkspaceService& AppRuntime::workspace() noexcept
  {
    return _implPtr->workspaceService;
  }

  ViewService& AppRuntime::views() noexcept
  {
    return _implPtr->viewService;
  }

  ConfigStore& AppRuntime::workspaceConfigStore() noexcept
  {
    return *_implPtr->workspaceConfigStorePtr;
  }

  ConfigStore& AppRuntime::playbackSessionConfigStore() noexcept
  {
    return *_implPtr->playbackSessionConfigStore;
  }

  Result<> AppRuntime::savePlaybackSession()
  {
    return _implPtr->playbackSessionPersistencePtr->checkpoint();
  }

  Result<PlaybackSessionRestoreResult> AppRuntime::restorePlaybackSession()
  {
    auto restoredRes = Result<PlaybackSessionRestoreResult>{};
    auto const accepted = _implPtr->playbackPtr->runSynchronousCommand(
      [this, &restoredRes]
      {
        restoredRes = _implPtr->playbackSessionPersistencePtr->restore();
        return restoredRes && restoredRes->restored;
      });

    if (!accepted)
    {
      return makeError(
        Error::Code::InvalidState, "Cannot restore playback while another playback command is active or pending");
    }

    if (!restoredRes)
    {
      return std::unexpected{restoredRes.error()};
    }

    return restoredRes;
  }

  Result<> AppRuntime::discardRestorablePlaybackSession()
  {
    return _implPtr->playbackSessionPersistencePtr->discardRestorableSession();
  }

  void AppRuntime::startPlaybackSessionPersistence()
  {
    _implPtr->playbackSessionPersistencePtr->start();
  }

  void AppRuntime::sealPlaybackSessionPersistenceWrites()
  {
    _implPtr->playbackSessionPersistencePtr->sealWrites();
  }

  Result<> AppRuntime::retirePlaybackSessionForLibrarySwitch()
  {
    return _implPtr->playbackSessionPersistencePtr->retireForLibrarySwitch();
  }

  void AppRuntime::reloadAllTracks()
  {
    sources().reloadAllTracks();
  }

  Result<TrackId> AppRuntime::playSelectionInFocusedView()
  {
    auto const focus = _implPtr->workspaceService.snapshot();

    if (focus.activeViewId == kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, "No track view is focused");
    }

    auto const stateRes = _implPtr->viewService.findTrackListState(focus.activeViewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    if (stateRes->selection.empty())
    {
      return makeError(Error::Code::NotFound, "The focused track view has no selection");
    }

    auto const trackId = stateRes->selection.front();

    if (auto const playedRes = _implPtr->playbackPtr->commands().startFromView(focus.activeViewId, trackId); !playedRes)
    {
      return std::unexpected{playedRes.error()};
    }

    return trackId;
  }

  Result<> AppRuntime::jumpToAlbum(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::InvalidInput, "Cannot reveal an invalid track id");
    }

    auto const* albums = builtinTrackPresentationPreset("albums");

    if (albums == nullptr)
    {
      return makeError(Error::Code::InvalidState, "The albums presentation is unavailable");
    }

    auto navigationRes = _implPtr->workspaceService.navigate(NavigationRequest{
      .target = GlobalViewKind::AllTracks,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = albums->spec,
        },
    });

    if (!navigationRes)
    {
      return std::unexpected{navigationRes.error()};
    }

    _implPtr->playbackPtr->commands().revealTrack(trackId, *navigationRes);
    return {};
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _implPtr->playbackBootstrap.addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
