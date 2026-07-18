// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/PlaybackSessionPersistence.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h> // NOLINT(misc-include-cleaner): unique_ptr<Executor> destruction needs the complete type.
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Player.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackService playbackService;
    PlaybackSequenceService playbackSequenceService;
    WorkspaceService workspaceService;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr;
    ConfigStore* playbackSessionConfigStore = nullptr;
    std::shared_ptr<PlaybackSessionPersistence> playbackSessionPersistencePtr;

    Impl(AppRuntime& runtime,
         std::unique_ptr<ConfigStore> workspaceConfigPtr,
         ConfigStore* playbackSessionConfigStoreValue)
      : viewService{runtime.async().callbackExecutor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.async().callbackExecutor(),
                        runtime.musicLibrary(),
                        runtime.notifications(),
                        std::make_unique<audio::Player>(runtime.async().callbackExecutor())}
      , playbackSequenceService{runtime.async().callbackExecutor(),
                                viewService,
                                runtime.sources(),
                                runtime.musicLibrary(),
                                playbackService,
                                runtime.notifications(),
                                runtime.async()}
      , workspaceService{runtime.async().callbackExecutor(), viewService, runtime.library().changes()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
      , playbackSessionConfigStore{playbackSessionConfigStoreValue != nullptr ? playbackSessionConfigStoreValue
                                                                              : workspaceConfigStorePtr.get()}
      , playbackSessionPersistencePtr{std::make_shared<PlaybackSessionPersistence>(*playbackSessionConfigStore,
                                                                                   runtime.library(),
                                                                                   playbackSequenceService,
                                                                                   playbackService,
                                                                                   runtime.async())}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      std::ignore = playbackSessionPersistencePtr->shutdown();
      // Join playback callback producers while every consumer is still alive.
      playbackService.shutdown();
    }
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime{std::move(dependencies.executorPtr),
                  std::move(dependencies.musicRoot),
                  std::move(dependencies.databasePath),
                  dependencies.musicLibraryMapSize,
                  dependencies.sleeper,
                  std::move(dependencies.asyncExceptionHandler)}
    , _implPtr{std::make_unique<Impl>(*this,
                                      std::move(dependencies.workspaceConfigStorePtr),
                                      dependencies.playbackSessionConfigStore)}
  {
  }

  AppRuntime::~AppRuntime() = default;

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _implPtr->playbackService;
  }

  PlaybackSequenceService& AppRuntime::playbackSequence() noexcept
  {
    return _implPtr->playbackSequenceService;
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
    _implPtr->playbackSessionPersistencePtr->start();
    auto restored = _implPtr->playbackSessionPersistencePtr->restore();

    if (!restored)
    {
      return std::unexpected{restored.error()};
    }

    return PlaybackSessionRestoreResult{
      .restored = restored->restored,
      .trackId = restored->trackId,
      .sourceListId = restored->sourceListId,
    };
  }

  Result<> AppRuntime::discardRestorablePlaybackSession()
  {
    return _implPtr->playbackSessionPersistencePtr->discardRestorableSession();
  }

  async::Subscription AppRuntime::onPlaybackSessionDirty(std::move_only_function<void()> handler)
  {
    return _implPtr->playbackSessionPersistencePtr->onDirty(std::move(handler));
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

    try
    {
      auto const state = _implPtr->viewService.trackListState(focus.activeViewId);

      if (state.selection.empty())
      {
        return makeError(Error::Code::NotFound, "The focused track view has no selection");
      }

      auto const trackId = state.selection.front();

      if (auto const played = _implPtr->playbackSequenceService.playFromView(focus.activeViewId, trackId); !played)
      {
        return std::unexpected{played.error()};
      }

      return trackId;
    }
    catch (std::exception const& error)
    {
      return makeError(Error::Code::Generic, error.what());
    }
  }

  Result<WorkspaceCommitReceipt> AppRuntime::jumpToAlbum(TrackId const trackId)
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

    auto navigation = _implPtr->workspaceService.navigateTo(
      GlobalViewKind::AllTracks, {.recordHistory = true, .optPresentation = albums->spec});

    if (!navigation)
    {
      return std::unexpected{navigation.error()};
    }

    _implPtr->playbackService.revealTrack(trackId, navigation->activeViewId);
    return *navigation;
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _implPtr->playbackService.addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
