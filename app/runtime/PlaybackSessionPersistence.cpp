// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionPersistence.h"

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "runtime/playback/PlaybackCursorSession.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace ao::rt
{
  namespace
  {
    class [[nodiscard]] RestoreScope final
    {
    public:
      explicit RestoreScope(bool& restoring) noexcept
        : _restoring{restoring}, _previous{restoring}
      {
        _restoring = true;
      }

      ~RestoreScope() { _restoring = _previous; }

      RestoreScope(RestoreScope const&) = delete;
      RestoreScope& operator=(RestoreScope const&) = delete;
      RestoreScope(RestoreScope&&) = delete;
      RestoreScope& operator=(RestoreScope&&) = delete;

    private:
      bool& _restoring;
      bool _previous = false;
    };

    PlaybackSessionState snapshotState(PlaybackLaunchSpec launchSpec,
                                       TrackId const currentTrackId,
                                       std::size_t const anchorIndex,
                                       std::chrono::milliseconds const elapsed,
                                       PlaybackSnapshot const& snapshot)
    {
      return PlaybackSessionState{
        .sourceListId = launchSpec.sourceListId,
        .quickFilterExpression = std::move(launchSpec.quickFilterExpression),
        .sortBy = std::move(launchSpec.order.sortBy),
        .currentTrackId = currentTrackId,
        .anchorIndex = static_cast<std::uint64_t>(anchorIndex),
        .positionMs = static_cast<std::uint64_t>(std::max(elapsed, std::chrono::milliseconds{0}).count()),
        .shuffleMode = snapshot.succession.shuffle,
        .repeatMode = snapshot.succession.repeat,
        .volume = snapshot.transport.volume.level,
        .muted = snapshot.transport.volume.muted,
      };
    }
  } // namespace

  PlaybackSessionPersistence::PlaybackSessionPersistence(ConfigStore& config,
                                                         Library& library,
                                                         PlaybackSuccession& succession,
                                                         PlaybackTransport& playbackTransport,
                                                         PlaybackService& playback,
                                                         async::Runtime& asyncRuntime)
    : _config{config}
    , _library{library}
    , _succession{succession}
    , _playbackTransport{playbackTransport}
    , _playback{playback}
    , _asyncRuntime{asyncRuntime}
  {
  }

  PlaybackSessionPersistence::~PlaybackSessionPersistence()
  {
    cancelScheduledSave();
  }

  void PlaybackSessionPersistence::ensureStarted()
  {
    if (_started || _shuttingDown)
    {
      return;
    }

    _started = true;
    _lastSnapshot = _playback.snapshot();
    _successionStateSubscription = _succession.onRestorableStateChanged([this] noexcept { requestDebouncedSave(); });
    _snapshotSubscription =
      _playback.events().onSnapshot([this](PlaybackSnapshot const& snapshot) noexcept { handleSnapshot(snapshot); });
  }

  Result<> PlaybackSessionPersistence::checkpoint()
  {
    if (_shuttingDown || _restoring)
    {
      return {};
    }

    ensureStarted();
    cancelScheduledSave();
    return save();
  }

  void PlaybackSessionPersistence::checkpointBestEffort()
  {
    if (auto const savedRes = checkpoint(); !savedRes)
    {
      APP_LOG_WARN("Playback session checkpoint failed: {}", savedRes.error().message);
    }
  }

  Result<> PlaybackSessionPersistence::shutdown()
  {
    if (_shuttingDown)
    {
      return {};
    }

    auto const shouldSave = _started;
    _shuttingDown = true;
    _successionStateSubscription.reset();
    _snapshotSubscription.reset();
    cancelScheduledSave();
    return shouldSave ? save() : Result<>{};
  }

  bool PlaybackSessionPersistence::hasActiveSession() const
  {
    return _succession.hasActivePlaybackSession();
  }

  void PlaybackSessionPersistence::handleSnapshot(PlaybackSnapshot const& snapshot)
  {
    auto const previous = _lastSnapshot;
    _lastSnapshot = snapshot;

    if (_restorePublicationPending)
    {
      // AppRuntime commits the lower-layer restore as one synchronous command
      // snapshot after the restore transaction returns. Adopt that publication
      // as the restored baseline without treating it as a new persistence change.
      _restorePublicationPending = false;
      return;
    }

    auto const subjectChanged =
      snapshot.transport.nowPlaying.trackId != previous.transport.nowPlaying.trackId ||
      snapshot.transport.nowPlaying.sourceListId != previous.transport.nowPlaying.sourceListId;
    auto const committedPositionChanged = snapshot.transport.finalSeekRevision != previous.transport.finalSeekRevision;
    auto const volumeChanged = snapshot.transport.volume.level != previous.transport.volume.level ||
                               snapshot.transport.volume.muted != previous.transport.volume.muted;

    if (volumeChanged && !committedPositionChanged)
    {
      requestDebouncedSave();
    }

    if (committedPositionChanged && _sessionDiscarded && hasActiveSession())
    {
      _sessionDiscarded = false;
    }

    auto const settledTransport = snapshot.transport.transport != previous.transport.transport &&
                                  (snapshot.transport.transport == audio::Transport::Paused ||
                                   snapshot.transport.transport == audio::Transport::Idle);

    if (_started && (subjectChanged || committedPositionChanged || settledTransport))
    {
      checkpointBestEffort();
    }
  }

  bool PlaybackSessionPersistence::hasRestorableSession() const
  {
    auto launchSpec = PlaybackLaunchSpec{};
    auto trackId = kInvalidTrackId;
    std::size_t anchorIndex = 0;
    return _succession.capturePlaybackSessionSnapshot(launchSpec, trackId, anchorIndex);
  }

  void PlaybackSessionPersistence::requestDebouncedSave()
  {
    if (auto const active = hasActiveSession();
        !_started || _shuttingDown || _restoring || !hasRestorableSession() || (_sessionDiscarded && !active))
    {
      return;
    }

    _sessionDiscarded = false;
    scheduleSave(kSaveDebounceDelay);
  }

  void PlaybackSessionPersistence::scheduleSave(Delay const delay)
  {
    cancelScheduledSave();
    _scheduledTask = _asyncRuntime.spawnCancellable(
      [asyncRuntime = &_asyncRuntime, owner = this, delay](std::stop_token const stopToken)
      { return waitForScheduledSave(asyncRuntime, owner, delay, stopToken); });
  }

  async::Task<void> PlaybackSessionPersistence::waitForScheduledSave(async::Runtime* asyncRuntime,
                                                                     PlaybackSessionPersistence* owner,
                                                                     Delay const delay,
                                                                     std::stop_token const stopToken)
  {
    co_await asyncRuntime->sleepFor(delay, stopToken);
    co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);
    owner->handleScheduledSave();
  }

  void PlaybackSessionPersistence::handleScheduledSave()
  {
    if (_shuttingDown)
    {
      return;
    }

    checkpointBestEffort();
  }

  void PlaybackSessionPersistence::cancelScheduledSave() noexcept
  {
    _scheduledTask.reset();
  }

  Result<> PlaybackSessionPersistence::save()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is in progress");
    }

    if (_sessionDiscarded)
    {
      return {};
    }

    auto launchSpec = PlaybackLaunchSpec{};
    auto currentTrackId = kInvalidTrackId;
    std::size_t anchorIndex = 0;

    if (!_succession.capturePlaybackSessionSnapshot(launchSpec, currentTrackId, anchorIndex))
    {
      return {};
    }

    auto const& snapshot = _playback.snapshot();
    auto const transportSession = _playbackTransport.playbackTransportSessionState();
    auto const elapsed =
      std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(transportSession.positionMs)};
    auto const publicSubjectMatches =
      snapshot.transport.nowPlaying.trackId == currentTrackId && snapshot.succession.currentTrackId == currentTrackId;

    if (transportSession.trackId == kInvalidTrackId || transportSession.trackId != currentTrackId ||
        (hasActiveSession() && !publicSubjectMatches))
    {
      return makeError(Error::Code::InvalidState, "Playback cursor and transport current tracks disagree during save");
    }

    auto const session = snapshotState(std::move(launchSpec), currentTrackId, anchorIndex, elapsed, snapshot);

    return _config.save(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{});
  }

  Result<PlaybackSessionRestoreResult> PlaybackSessionPersistence::restore()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is already in progress");
    }

    ensureStarted();
    auto loaded = PlaybackSessionState{};
    auto const loadedSessionRes = _config.load(kPlaybackSessionConfigGroup, loaded, PlaybackSessionYamlSchema{});

    if (!loadedSessionRes)
    {
      return std::unexpected{loadedSessionRes.error()};
    }

    if (!*loadedSessionRes)
    {
      return PlaybackSessionRestoreResult{};
    }

    {
      auto reader = _library.reader();
      auto const currentExists = reader.containsTrack(loaded.currentTrackId);
      auto const sourceExists = loaded.sourceListId == kAllTracksListId || reader.listNode(loaded.sourceListId);

      if (!sourceExists && !currentExists)
      {
        return PlaybackSessionRestoreResult{};
      }

      auto launchSpec = PlaybackLaunchSpec{
        .sourceListId = loaded.sourceListId,
        .quickFilterExpression = loaded.quickFilterExpression,
        .order = TrackOrderSpec{.sortBy = loaded.sortBy},
      };

      if (!sourceExists)
      {
        launchSpec.sourceListId = kAllTracksListId;
        launchSpec.quickFilterExpression.clear();
      }

      auto currentTrackId = loaded.currentTrackId;
      auto positionMs = loaded.positionMs;
      auto candidateRes = _succession.preparePlaybackSessionRestore(launchSpec,
                                                                    currentTrackId,
                                                                    static_cast<std::size_t>(loaded.anchorIndex),
                                                                    loaded.shuffleMode,
                                                                    loaded.repeatMode);

      if (!candidateRes)
      {
        return std::unexpected{candidateRes.error()};
      }

      if (!currentExists)
      {
        auto optReplacementIndex = std::optional<std::size_t>{};
        auto const savedAnchor = static_cast<std::size_t>(loaded.anchorIndex);

        if (savedAnchor < (*candidateRes)->projectionSize())
        {
          optReplacementIndex = savedAnchor;
        }
        else if (loaded.repeatMode == RepeatMode::All && (*candidateRes)->projectionSize() != 0)
        {
          optReplacementIndex = 0;
        }

        if (!optReplacementIndex)
        {
          return PlaybackSessionRestoreResult{};
        }

        currentTrackId = (*candidateRes)->trackIdAt(*optReplacementIndex);
        positionMs = 0;
        candidateRes = _succession.preparePlaybackSessionRestore(
          launchSpec, currentTrackId, *optReplacementIndex, loaded.shuffleMode, loaded.repeatMode);

        if (!candidateRes)
        {
          return std::unexpected{candidateRes.error()};
        }
      }

      auto const result = PlaybackSessionRestoreResult{
        .restored = true,
        .trackId = currentTrackId,
        .sourceListId = launchSpec.sourceListId,
      };
      auto const transport = PlaybackTransportSessionState{
        .sourceListId = launchSpec.sourceListId,
        .trackId = currentTrackId,
        .positionMs = positionMs,
        .volume = loaded.volume,
        .muted = loaded.muted,
      };

      auto restoredRes = [&] -> Result<>
      {
        auto const restoring = RestoreScope{_restoring};
        auto restoredElapsedRes = _playbackTransport.restorePlaybackTransport(transport);

        if (!restoredElapsedRes)
        {
          return std::unexpected{restoredElapsedRes.error()};
        }

        _restorePublicationPending = true;
        _succession.commitPlaybackSessionRestore(
          std::move(*candidateRes), loaded.shuffleMode, loaded.repeatMode, *restoredElapsedRes);

        _sessionDiscarded = false;

        return {};
      }();

      if (!restoredRes)
      {
        return std::unexpected{restoredRes.error()};
      }

      return result;
    }
  }

  Result<> PlaybackSessionPersistence::discardRestorableSession()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is in progress");
    }

    ensureStarted();
    cancelScheduledSave();

    if (auto const removedRes = _config.removeGroup(kPlaybackSessionConfigGroup); !removedRes)
    {
      return std::unexpected{removedRes.error()};
    }

    _succession.discardPlaybackSessionSnapshot();
    _playbackTransport.discardPlaybackTransportSnapshot();
    _sessionDiscarded = true;
    return {};
  }
} // namespace ao::rt
