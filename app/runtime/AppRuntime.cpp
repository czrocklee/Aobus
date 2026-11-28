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
    PlaybackSessionPersistence playbackSessionPersistence;
    AppRuntime services;
    bool stopped = false;

    Impl(CoreRuntime&& coreValue,
         std::unique_ptr<ConfigStore> workspaceConfigPtr,
         ConfigStore* playbackSessionConfigStoreValue)
      : core{std::move(coreValue)}
      , resourceByteCache{core.async(),
                          [finalCore = &core](ResourceId resourceId, std::stop_token stopToken)
                          { return finalCore->readInteractiveResourceBytesAsync(resourceId, stopToken); }}
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
      , playback{playbackBootstrap.createPlaybackService(core.async().callbackExecutor(),
                                                         playbackSuccession,
                                                         core.musicLibrary(),
                                                         core.library().changes())}
      , workspaceService{core.async().callbackExecutor(), viewService, core.musicLibrary(), core.library().changes()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
      , playbackSessionPersistence{playbackSessionConfigStoreValue != nullptr ? *playbackSessionConfigStoreValue
                                                                              : *workspaceConfigStorePtr,
                                   core.library(),
                                   playbackSuccession,
                                   playbackTransport,
                                   playback,
                                   core.async()}
      , services{*this}
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

  Result<AppRuntimeOwner> AppRuntimeOwner::open(AppRuntimeDependencies dependencies)
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
                                       dependencies.musicLibraryDurability,
                                       dependencies.sleeper,
                                       dependencies.textOrderingPolicy,
                                       dependencies.completionAliasPolicy);

    if (!coreRes)
    {
      return std::unexpected{coreRes.error()};
    }

    return AppRuntimeOwner{std::make_unique<AppRuntime::Impl>(
      std::move(*coreRes), std::move(dependencies.workspaceConfigStorePtr), dependencies.playbackSessionConfigStore)};
  }

  AppRuntimeOwner::AppRuntimeOwner(std::unique_ptr<AppRuntime::Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  AppRuntimeOwner::~AppRuntimeOwner() = default;
  AppRuntimeOwner::AppRuntimeOwner(AppRuntimeOwner&&) noexcept = default;

  AppRuntime& AppRuntimeOwner::runtime() & noexcept
  {
    return _implPtr->services;
  }

  LibraryCommands& AppRuntimeOwner::libraryCommands() & noexcept
  {
    return _implPtr->core.libraryCommands();
  }

  LibraryJobs& AppRuntimeOwner::libraryJobs() & noexcept
  {
    return _implPtr->core.libraryJobs();
  }

  void AppRuntimeOwner::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }
  }

  Library const& AppRuntime::library() const noexcept
  {
    return _impl.core.library();
  }

  LibraryJobs const& AppRuntime::jobProgress() const noexcept
  {
    return _impl.core.libraryJobs();
  }

  async::Runtime& AppRuntime::async() noexcept
  {
    return _impl.core.async();
  }

  TrackSourceCache& AppRuntime::sources() noexcept
  {
    return _impl.core.sources();
  }

  NotificationService& AppRuntime::notifications() noexcept
  {
    return _impl.core.notifications();
  }

  CompletionService& AppRuntime::completion() noexcept
  {
    return _impl.core.completion();
  }

  TextOrderingPolicy const* AppRuntime::textOrderingPolicy() const noexcept
  {
    return _impl.core.textOrderingPolicy();
  }

  std::filesystem::path const& AppRuntime::musicRoot() const noexcept
  {
    return _impl.core.musicRoot();
  }

  ResourceByteMemoryCache& AppRuntime::resourceBytes() noexcept
  {
    return _impl.resourceByteCache;
  }

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _impl.playback;
  }

  WorkspaceService& AppRuntime::workspace() noexcept
  {
    return _impl.workspaceService;
  }

  ViewService& AppRuntime::views() noexcept
  {
    return _impl.viewService;
  }

  // Mutable config access belongs to a non-const service borrow.
  ConfigStore& AppRuntime::workspaceConfigStore() noexcept // NOLINT(readability-make-member-function-const)
  {
    return *_impl.workspaceConfigStorePtr;
  }

  Result<> AppRuntime::savePlaybackSession()
  {
    return _impl.playbackSessionPersistence.checkpoint();
  }

  Result<PlaybackSessionRestoreResult> AppRuntime::restorePlaybackSession()
  {
    auto restoredRes = Result<PlaybackSessionRestoreResult>{};
    auto const accepted = _impl.playback.tryRunSynchronousCommand(
      [this, &restoredRes]
      {
        restoredRes = _impl.playbackSessionPersistence.restore();
        return restoredRes && restoredRes->restored;
      });

    if (!accepted)
    {
      return makeError(
        Error::Code::InvalidState, "Cannot restore playback while another playback command is active or pending");
    }

    return restoredRes;
  }

  Result<> AppRuntime::discardRestorablePlaybackSession()
  {
    return _impl.playbackSessionPersistence.discardRestorableSession();
  }

  void AppRuntime::startPlaybackSessionPersistence()
  {
    _impl.playbackSessionPersistence.start();
  }

  void AppRuntime::sealPlaybackSessionPersistenceWrites()
  {
    _impl.playbackSessionPersistence.sealWrites();
  }

  Result<> AppRuntime::retirePlaybackSessionForLibrarySwitch()
  {
    return _impl.playbackSessionPersistence.retireForLibrarySwitch();
  }

  Result<TrackId> AppRuntime::playSelectionInFocusedView()
  {
    auto const focus = _impl.workspaceService.snapshot();

    if (focus.activeViewId == kInvalidViewId)
    {
      return makeError(Error::Code::InvalidState, "No track view is focused");
    }

    auto const trackId = _impl.viewService.firstVisibleSelectedTrack(focus.activeViewId);

    if (trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::NotFound, "The focused track view has no playback target");
    }

    if (auto const playedRes = _impl.playback.commands().startFromView(focus.activeViewId, trackId); !playedRes)
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

    auto navigationRes = _impl.workspaceService.navigate(NavigationRequest{
      .target = GlobalViewKind::AllTracks,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec,
        },
    });

    if (!navigationRes)
    {
      return std::unexpected{navigationRes.error()};
    }

    _impl.playback.commands().revealTrack(trackId, *navigationRes);
    return {};
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _impl.playbackBootstrap.addProvider(std::move(providerPtr));
  }

  void AppRuntime::setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const> policyPtr)
  {
    _impl.core.setTextOrderingPolicy(policyPtr);
    _impl.viewService.setTextOrderingPolicy(std::move(policyPtr));
  }
} // namespace ao::rt
