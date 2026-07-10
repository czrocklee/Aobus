// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h> // NOLINT(misc-include-cleaner): unique_ptr<Executor> destruction needs the complete type.
#include <ao/async/Runtime.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    bool isValidShuffleMode(ShuffleMode const mode) noexcept
    {
      switch (mode)
      {
        case ShuffleMode::Off:
        case ShuffleMode::On: return true;
      }

      return false;
    }

    bool isValidRepeatMode(RepeatMode const mode) noexcept
    {
      switch (mode)
      {
        case RepeatMode::Off:
        case RepeatMode::One:
        case RepeatMode::All: return true;
      }

      return false;
    }

    Result<> validatePlaybackSession(PlaybackSessionState const& session)
    {
      if (session.schemaVersion != kPlaybackSessionSchemaVersion)
      {
        return makeError(Error::Code::FormatRejected, "Unsupported playback session schema version");
      }

      if (session.queueTrackIds.empty())
      {
        return makeError(Error::Code::CorruptData, "Playback session queue is empty");
      }

      if (session.currentQueueIndex >= session.queueTrackIds.size())
      {
        return makeError(Error::Code::CorruptData, "Playback session current queue index is out of range");
      }

      if (session.trackId == kInvalidTrackId ||
          session.queueTrackIds[static_cast<std::size_t>(session.currentQueueIndex)] != session.trackId)
      {
        return makeError(Error::Code::CorruptData, "Playback session current track does not match its queue index");
      }

      if (std::ranges::contains(session.queueTrackIds, kInvalidTrackId))
      {
        return makeError(Error::Code::CorruptData, "Playback session queue contains an invalid track id");
      }

      auto const maxPosition = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());

      if (session.positionMs > maxPosition || !std::isfinite(session.volume) || session.volume < 0.0F ||
          session.volume > 1.0F || !isValidShuffleMode(session.shuffleMode) || !isValidRepeatMode(session.repeatMode))
      {
        return makeError(Error::Code::CorruptData, "Playback session contains invalid playback values");
      }

      return {};
    }

    struct SanitizedPlaybackQueue final
    {
      std::vector<TrackId> trackIds{};
      std::size_t currentIndex = 0;
      ListId sourceListId = kInvalidListId;
    };

    Result<SanitizedPlaybackQueue> sanitizePlaybackQueue(PlaybackSessionState const& session, Library& library)
    {
      auto reader = library.reader();
      auto sourceListId = session.sourceListId;

      if (sourceListId == kInvalidListId ||
          (sourceListId != kAllTracksListId && !reader.listNode(sourceListId).has_value()))
      {
        sourceListId = kAllTracksListId;
      }

      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(session.queueTrackIds.size());
      auto optCurrentIndex = std::optional<std::size_t>{};

      for (std::size_t index = 0; index < session.queueTrackIds.size(); ++index)
      {
        auto const trackId = session.queueTrackIds[index];

        if (!reader.containsTrack(trackId))
        {
          continue;
        }

        if (index == session.currentQueueIndex)
        {
          optCurrentIndex = trackIds.size();
        }

        trackIds.push_back(trackId);
      }

      if (!optCurrentIndex)
      {
        return makeError(Error::Code::NotFound, "Playback session current track no longer exists");
      }

      return SanitizedPlaybackQueue{
        .trackIds = std::move(trackIds), .currentIndex = *optCurrentIndex, .sourceListId = sourceListId};
    }
  } // namespace

  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackService playbackService;
    PlaybackQueueService playbackQueueService;
    WorkspaceService workspaceService;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr;

    Impl(AppRuntime& runtime, std::unique_ptr<ConfigStore> workspaceConfigPtr)
      : viewService{runtime.async().callbackExecutor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.async().callbackExecutor(),
                        viewService,
                        runtime.musicLibrary(),
                        runtime.notifications()}
      , playbackQueueService{runtime.async().callbackExecutor(), playbackService, runtime.notifications()}
      , workspaceService{viewService, playbackService, runtime.library().changes(), runtime.musicLibrary()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() = default;
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime{std::move(dependencies.executorPtr),
                  std::move(dependencies.musicRoot),
                  std::move(dependencies.databasePath),
                  dependencies.musicLibraryMapSize}
    , _implPtr{std::make_unique<Impl>(*this, std::move(dependencies.workspaceConfigStorePtr))}
  {
  }

  AppRuntime::~AppRuntime() = default;

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _implPtr->playbackService;
  }

  PlaybackQueueService& AppRuntime::playbackQueue() noexcept
  {
    return _implPtr->playbackQueueService;
  }

  WorkspaceService& AppRuntime::workspace() noexcept
  {
    return _implPtr->workspaceService;
  }

  ViewService& AppRuntime::views() noexcept
  {
    return _implPtr->viewService;
  }

  ConfigStore& AppRuntime::configStore() noexcept
  {
    return *_implPtr->workspaceConfigStorePtr;
  }

  Result<> AppRuntime::savePlaybackSession()
  {
    auto session = _implPtr->playbackService.playbackSessionState();

    if (session.trackId == kInvalidTrackId)
    {
      return {};
    }

    auto const queueState = _implPtr->playbackQueueService.playbackSessionQueueState();

    if (!queueState.optCurrentIndex || *queueState.optCurrentIndex >= queueState.trackIds.size())
    {
      return makeError(Error::Code::InvalidState, "Cannot save playback without an active or restorable queue");
    }

    auto const queueTrackId = queueState.trackIds[*queueState.optCurrentIndex];

    if (queueTrackId != session.trackId)
    {
      return makeError(Error::Code::InvalidState, "Playback and queue current tracks disagree during session save");
    }

    session.schemaVersion = kPlaybackSessionSchemaVersion;
    session.queueTrackIds = queueState.trackIds;
    session.currentQueueIndex = static_cast<std::uint64_t>(*queueState.optCurrentIndex);
    session.sourceListId = queueState.sourceListId;

    if (auto const saved = _implPtr->workspaceConfigStorePtr->saveResult(kPlaybackSessionConfigGroup, session); !saved)
    {
      return saved;
    }

    return _implPtr->workspaceConfigStorePtr->flush();
  }

  Result<PlaybackSessionRestoreResult> AppRuntime::restorePlaybackSession()
  {
    auto const containsSession = _implPtr->workspaceConfigStorePtr->contains(kPlaybackSessionConfigGroup);

    if (!containsSession)
    {
      return std::unexpected{containsSession.error()};
    }

    if (!*containsSession)
    {
      return PlaybackSessionRestoreResult{};
    }

    auto session = PlaybackSessionState{};

    if (auto const loaded = _implPtr->workspaceConfigStorePtr->load(kPlaybackSessionConfigGroup, session); !loaded)
    {
      if (loaded.error().code == Error::Code::NotFound)
      {
        return PlaybackSessionRestoreResult{};
      }

      return std::unexpected{loaded.error()};
    }

    if (auto const valid = validatePlaybackSession(session); !valid)
    {
      return std::unexpected{valid.error()};
    }

    auto queueResult = sanitizePlaybackQueue(session, library());

    if (!queueResult)
    {
      return std::unexpected{queueResult.error()};
    }

    auto queue = std::move(*queueResult);
    session.queueTrackIds = queue.trackIds;
    session.currentQueueIndex = queue.currentIndex;
    session.sourceListId = queue.sourceListId;
    session.trackId = queue.trackIds[queue.currentIndex];
    bool queueRestoreBegan = false;
    auto restored = _implPtr->playbackService.restorePlaybackSession(
      session,
      [this,
       trackIds = std::move(queue.trackIds),
       currentIndex = queue.currentIndex,
       sourceListId = queue.sourceListId,
       &queueRestoreBegan] mutable
      {
        _implPtr->playbackQueueService.beginPlaybackSessionRestore(std::move(trackIds), currentIndex, sourceListId);
        queueRestoreBegan = true;
      });

    if (!restored)
    {
      if (queueRestoreBegan)
      {
        _implPtr->playbackQueueService.cancelPlaybackSessionRestore();
      }

      return std::unexpected{restored.error()};
    }

    _implPtr->playbackQueueService.finishPlaybackSessionRestore();
    return PlaybackSessionRestoreResult{
      .restored = true, .trackId = session.trackId, .sourceListId = session.sourceListId};
  }

  void AppRuntime::reloadAllTracks()
  {
    sources().reloadAllTracks();
  }

  TrackId AppRuntime::playSelectionInFocusedView()
  {
    auto const focus = _implPtr->workspaceService.layoutState();

    if (focus.activeViewId == rt::kInvalidViewId)
    {
      return kInvalidTrackId;
    }

    return _implPtr->playbackService.playSelectionInView(focus.activeViewId);
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _implPtr->playbackService.addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
