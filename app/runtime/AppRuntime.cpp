// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppRuntime.h>

#include "runtime/PlaybackSessionPersistence.h"
#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
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
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <expected>
#include <memory>
#include <stop_token>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    CoreRuntime core;
    ResourceByteMemoryCache resourceByteCache;
    ViewService viewService;
    PlaybackTransport playbackTransport;
    PlaybackSuccession playbackSuccession;
    PlaybackBootstrap playbackBootstrap;
    PlaybackService playback;
    WorkspaceService workspaceService;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr;
    ConfigStore& playbackSessionStore;
    PlaybackSessionPersistence playbackSessionPersistence;
    bool stopped = false;

    template<typename ReadBytesFactory>
    Impl(CoreRuntime&& coreValue,
         ReadBytesFactory&& makeReadResourceBytes,
         std::unique_ptr<ConfigStore> workspaceConfigPtr,
         ConfigStore* playbackSessionConfigStoreValue)
      : core{std::move(coreValue)}
      , resourceByteCache{core.async(), std::forward<ReadBytesFactory>(makeReadResourceBytes)(core)}
      , viewService{core.async().callbackExecutor(),
                    core.musicLibrary(),
                    core.sources(),
                    core.library().changes(),
                    core.textOrderingPolicy()}
      , playbackTransport{core.async().callbackExecutor(),
                          core.musicLibrary(),
                          core.notifications(),
                          std::make_unique<audio::Player>(core.async())}
      , playbackSuccession{core.async().callbackExecutor(),
                           viewService,
                           core.sources(),
                           core.musicLibrary(),
                           playbackTransport,
                           core.notifications(),
                           core.async()}
      , playbackBootstrap{playbackTransport}
      , playback{playbackBootstrap.createPlaybackService(core.async().callbackExecutor(), playbackSuccession)}
      , workspaceService{core.async().callbackExecutor(), viewService, core.library().changes()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
      , playbackSessionStore{playbackSessionConfigStoreValue != nullptr ? *playbackSessionConfigStoreValue
                                                                        : *workspaceConfigStorePtr}
      , playbackSessionPersistence{playbackSessionStore,
                                   core.library(),
                                   playbackSuccession,
                                   playbackTransport,
                                   playback,
                                   core.async()}
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
      std::ignore = playbackSessionPersistence.shutdown();
      // Join playback callback producers while every consumer is still alive.
      playback.shutdown();
      playbackBootstrap.shutdown();
      core.shutdown();
    }
  };

  Result<AppRuntime> AppRuntime::create(AppRuntimeDependencies dependencies)
  {
    if (dependencies.executorPtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "AppRuntime requires an executor");
    }

    if (dependencies.workspaceConfigStorePtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "AppRuntime requires a workspace config store");
    }

    auto coreRes = CoreRuntime::create(std::move(dependencies.executorPtr),
                                       std::move(dependencies.musicRoot),
                                       std::move(dependencies.databasePath),
                                       std::move(dependencies.cacheDirectory),
                                       dependencies.musicLibraryPinnedMapBytes,
                                       dependencies.sleeper,
                                       dependencies.textOrderingPolicy,
                                       dependencies.completionAliasPolicy);

    if (!coreRes)
    {
      return std::unexpected{coreRes.error()};
    }

    return AppRuntime{
      std::move(*coreRes), std::move(dependencies.workspaceConfigStorePtr), dependencies.playbackSessionConfigStore};
  }

  AppRuntime::AppRuntime(CoreRuntime&& core,
                         std::unique_ptr<ConfigStore> workspaceConfigStorePtr,
                         ConfigStore* const playbackSessionConfigStore)
  {
    auto const makeReadResourceBytes = [](CoreRuntime& finalCore)
    {
      return ResourceByteMemoryCache::ReadBytes{
        [core = &finalCore](ResourceId const resourceId, std::stop_token const stopToken)
        { return core->readInteractiveResourceBytesAsync(resourceId, stopToken); }};
    };
    _implPtr = std::make_unique<Impl>(
      std::move(core), makeReadResourceBytes, std::move(workspaceConfigStorePtr), playbackSessionConfigStore);
  }

  AppRuntime::~AppRuntime() = default;
  AppRuntime::AppRuntime(AppRuntime&& other) noexcept = default;

  void AppRuntime::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }
  }

  Library const& AppRuntime::library() const noexcept
  {
    return _implPtr->core.library();
  }

  Library& AppRuntime::library() noexcept
  {
    return _implPtr->core.library();
  }

  async::Runtime& AppRuntime::async() noexcept
  {
    return _implPtr->core.async();
  }

  TrackSourceCache& AppRuntime::sources() noexcept
  {
    return _implPtr->core.sources();
  }

  NotificationService& AppRuntime::notifications() noexcept
  {
    return _implPtr->core.notifications();
  }

  CompletionService& AppRuntime::completion() noexcept
  {
    return _implPtr->core.completion();
  }

  TextOrderingPolicy const* AppRuntime::textOrderingPolicy() const noexcept
  {
    return _implPtr->core.textOrderingPolicy();
  }

  std::filesystem::path const& AppRuntime::musicRoot() const noexcept
  {
    return _implPtr->core.musicRoot();
  }

  ResourceByteMemoryCache& AppRuntime::resourceBytes() noexcept
  {
    return _implPtr->resourceByteCache;
  }

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _implPtr->playback;
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
    return _implPtr->playbackSessionStore;
  }

  Result<> AppRuntime::savePlaybackSession()
  {
    return _implPtr->playbackSessionPersistence.checkpoint();
  }

  Result<PlaybackSessionRestoreResult> AppRuntime::restorePlaybackSession()
  {
    auto restoredRes = Result<PlaybackSessionRestoreResult>{};
    auto const accepted = _implPtr->playback.runSynchronousCommand(
      [this, &restoredRes]
      {
        restoredRes = _implPtr->playbackSessionPersistence.restore();
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
    return _implPtr->playbackSessionPersistence.discardRestorableSession();
  }

  void AppRuntime::startPlaybackSessionPersistence()
  {
    _implPtr->playbackSessionPersistence.start();
  }

  void AppRuntime::sealPlaybackSessionPersistenceWrites()
  {
    _implPtr->playbackSessionPersistence.sealWrites();
  }

  Result<> AppRuntime::retirePlaybackSessionForLibrarySwitch()
  {
    return _implPtr->playbackSessionPersistence.retireForLibrarySwitch();
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

    if (auto const playedRes = _implPtr->playback.commands().startFromView(focus.activeViewId, trackId); !playedRes)
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

    _implPtr->playback.commands().revealTrack(trackId, *navigationRes);
    return {};
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _implPtr->playbackBootstrap.addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
