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
#include <ao/async/Signal.h>
#include <ao/async/Task.h>
#include <ao/audio/Transport.h>
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
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace ao::rt
{
  namespace
  {
    class [[nodiscard]] RestoreIntentTransaction final
    {
    public:
      explicit RestoreIntentTransaction(bool& restoring) noexcept
        : _restoring{restoring}, _previous{restoring}
      {
        _restoring = true;
      }

      ~RestoreIntentTransaction() { _restoring = _previous; }

      RestoreIntentTransaction(RestoreIntentTransaction const&) = delete;
      RestoreIntentTransaction& operator=(RestoreIntentTransaction const&) = delete;
      RestoreIntentTransaction(RestoreIntentTransaction&&) = delete;
      RestoreIntentTransaction& operator=(RestoreIntentTransaction&&) = delete;

    private:
      bool& _restoring;
      bool _previous = false;
    };

    void publishDirtySafely(async::Signal<>& signal) noexcept
    {
      try
      {
        signal.emit();
      }
      catch (std::exception const& error)
      {
        try
        {
          APP_LOG_ERROR("Playback session dirty observer threw: {}", error.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- observer containment must remain noexcept
        {
        }
      }
      catch (...)
      {
        try
        {
          APP_LOG_ERROR("Playback session dirty observer threw an unknown exception");
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- observer containment must remain noexcept
        {
        }
      }
    }

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
    , _lastSnapshot{_playback.snapshot()}
    , _intentPosition{_lastSnapshot.transport.elapsed}
    , _volumeIntent{_lastSnapshot.transport.volume.level}
    , _mutedIntent{_lastSnapshot.transport.volume.muted}
  {
    _successionIntentSubscription = _succession.onPersistenceIntentChanged(
      [this]
      {
        _intentPosition = _playbackTransport.elapsed();
        markDirty();
      });
    _snapshotSubscription =
      _playback.events().onSnapshot([this](PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  PlaybackSessionPersistence::~PlaybackSessionPersistence() = default;

  void PlaybackSessionPersistence::start()
  {
    if (_started || _shuttingDown)
    {
      return;
    }

    _started = true;

    if (_sessionRevision.dirty())
    {
      scheduleSave(ScheduledSave::DirtyDebounce, kDirtyDebounceDelay);
    }

    startPeriodicSave();
  }

  Result<> PlaybackSessionPersistence::checkpoint()
  {
    if (_shuttingDown || _restoring)
    {
      return {};
    }

    cancelScheduledSave();

    if (auto result = save(); !result)
    {
      scheduleRetry();
      return result;
    }

    handleSaveSucceeded();
    return {};
  }

  Result<> PlaybackSessionPersistence::shutdown()
  {
    if (_shuttingDown)
    {
      return {};
    }

    auto const shouldSave = _started;
    _shuttingDown = true;
    _successionIntentSubscription.reset();
    _snapshotSubscription.reset();
    cancelScheduledSave();
    _periodicTask.reset();
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
    bool persistenceIntentChanged = false;
    auto const hasCoherentSubject = snapshot.transport.nowPlaying.trackId != kInvalidTrackId &&
                                    snapshot.transport.nowPlaying.trackId == snapshot.succession.currentTrackId;

    if (hasCoherentSubject && snapshot.transport.elapsed != _intentPosition)
    {
      _intentPosition = snapshot.transport.elapsed;
      persistenceIntentChanged = true;
    }

    if (snapshot.transport.volume.level != _volumeIntent)
    {
      _volumeIntent = snapshot.transport.volume.level;
      persistenceIntentChanged = true;
    }

    if (snapshot.transport.volume.muted != _mutedIntent)
    {
      _mutedIntent = snapshot.transport.volume.muted;
      persistenceIntentChanged = true;
    }

    if (persistenceIntentChanged)
    {
      markDirty();
    }

    auto const subjectChanged =
      snapshot.transport.nowPlaying.trackId != previous.transport.nowPlaying.trackId ||
      snapshot.transport.nowPlaying.sourceListId != previous.transport.nowPlaying.sourceListId;
    auto const committedPositionChanged = snapshot.transport.elapsed != previous.transport.elapsed;
    auto const settledTransport = snapshot.transport.transport != previous.transport.transport &&
                                  (snapshot.transport.transport == audio::Transport::Paused ||
                                   snapshot.transport.transport == audio::Transport::Idle);

    if (_started && (subjectChanged || committedPositionChanged || settledTransport))
    {
      std::ignore = checkpoint();
    }
  }

  bool PlaybackSessionPersistence::hasRestorableSession() const
  {
    auto launchSpec = PlaybackLaunchSpec{};
    auto trackId = kInvalidTrackId;
    std::size_t anchorIndex = 0;
    return _succession.capturePlaybackSessionSnapshot(launchSpec, trackId, anchorIndex);
  }

  void PlaybackSessionPersistence::markDirty()
  {
    if (auto const active = hasActiveSession(); _restoring || !hasRestorableSession() || (_sessionDiscarded && !active))
    {
      return;
    }

    _sessionDiscarded = false;

    if (_sessionRevision.markDirty())
    {
      publishDirtySafely(_dirtySignal);

      if (_started && !_shuttingDown && _scheduledSave != ScheduledSave::Retry)
      {
        scheduleSave(ScheduledSave::DirtyDebounce, kDirtyDebounceDelay);
      }
    }
  }

  void PlaybackSessionPersistence::handleSaveSucceeded()
  {
    cancelScheduledSave();
    _nextRetryDelay = kInitialRetryDelay;

    if (_sessionRevision.dirty() && _started && !_shuttingDown)
    {
      scheduleSave(ScheduledSave::DirtyDebounce, kDirtyDebounceDelay);
    }
  }

  void PlaybackSessionPersistence::scheduleRetry()
  {
    if (!_started || _shuttingDown)
    {
      return;
    }

    auto const retryDelay = _nextRetryDelay;

    if (_nextRetryDelay >= kMaximumRetryDelay / 2)
    {
      _nextRetryDelay = kMaximumRetryDelay;
    }
    else
    {
      _nextRetryDelay = std::min(_nextRetryDelay * 2, kMaximumRetryDelay);
    }

    scheduleSave(ScheduledSave::Retry, retryDelay);
  }

  void PlaybackSessionPersistence::scheduleSave(ScheduledSave const kind, Delay const delay)
  {
    cancelScheduledSave();
    _scheduledSave = kind;
    auto const callbackGeneration = _scheduleGeneration;
    _scheduledTask = _asyncRuntime.spawnCancellable(
      [asyncRuntime = &_asyncRuntime, weakSelfPtr = weak_from_this(), delay, callbackGeneration, kind](
        std::stop_token const stopToken)
      { return waitForScheduledSave(asyncRuntime, weakSelfPtr, delay, callbackGeneration, kind, stopToken); });
  }

  async::Task<void> PlaybackSessionPersistence::waitForScheduledSave(
    async::Runtime* asyncRuntime,
    std::weak_ptr<PlaybackSessionPersistence> weakSelfPtr,
    Delay const delay,
    std::uint64_t const scheduleGeneration,
    ScheduledSave const kind,
    std::stop_token const stopToken)
  {
    co_await asyncRuntime->sleepFor(delay, stopToken);
    co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);

    if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
    {
      selfPtr->handleScheduledSave(scheduleGeneration, kind);
    }
  }

  void PlaybackSessionPersistence::handleScheduledSave(std::uint64_t const scheduleGeneration, ScheduledSave const kind)
  {
    if (_shuttingDown || scheduleGeneration != _scheduleGeneration || _scheduledSave != kind)
    {
      return;
    }

    std::ignore = checkpoint();
  }

  void PlaybackSessionPersistence::cancelScheduledSave() noexcept
  {
    ++_scheduleGeneration;
    _scheduledSave = ScheduledSave::None;
    _scheduledTask.reset();
  }

  void PlaybackSessionPersistence::startPeriodicSave()
  {
    _periodicTask = _asyncRuntime.spawnCancellable(
      [asyncRuntime = &_asyncRuntime, weakSelfPtr = weak_from_this()](std::stop_token const stopToken)
      { return runPeriodicSave(asyncRuntime, weakSelfPtr, stopToken); });
  }

  async::Task<void> PlaybackSessionPersistence::runPeriodicSave(async::Runtime* asyncRuntime,
                                                                std::weak_ptr<PlaybackSessionPersistence> weakSelfPtr,
                                                                std::stop_token const stopToken)
  {
    while (true)
    {
      co_await asyncRuntime->sleepFor(kPeriodicSaveInterval, stopToken);
      co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);

      {
        auto const selfPtr = weakSelfPtr.lock();

        if (!selfPtr || selfPtr->_shuttingDown)
        {
          co_return;
        }

        if (selfPtr->_playback.snapshot().transport.transport == audio::Transport::Playing)
        {
          std::ignore = selfPtr->checkpoint();
        }
      }

      co_await asyncRuntime->resumeOnWorker(stopToken);
    }
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

    auto const snapshot = _playback.snapshot();
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

    auto const capturedRevision = _sessionRevision.capture();
    auto const session = snapshotState(std::move(launchSpec), currentTrackId, anchorIndex, elapsed, snapshot);

    if (auto const saved = _config.save(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{}); !saved)
    {
      return saved;
    }

    _sessionRevision.acknowledge(capturedRevision);

    return {};
  }

  Result<PlaybackSessionPersistenceRestoreResult> PlaybackSessionPersistence::restore()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is already in progress");
    }

    auto loaded = PlaybackSessionState{};
    auto const loadedSession = _config.load(kPlaybackSessionConfigGroup, loaded, PlaybackSessionYamlSchema{});

    if (!loadedSession)
    {
      return std::unexpected{loadedSession.error()};
    }

    if (!*loadedSession)
    {
      return PlaybackSessionPersistenceRestoreResult{};
    }

    try
    {
      auto reader = _library.reader();
      auto const currentExists = reader.containsTrack(loaded.currentTrackId);
      auto const sourceExists = loaded.sourceListId == kAllTracksListId || reader.listNode(loaded.sourceListId);

      if (!sourceExists && !currentExists)
      {
        return PlaybackSessionPersistenceRestoreResult{};
      }

      auto restoredState = loaded;
      auto launchSpec = PlaybackLaunchSpec{
        .sourceListId = loaded.sourceListId,
        .quickFilterExpression = loaded.quickFilterExpression,
        .order = TrackOrderSpec{.sortBy = loaded.sortBy},
      };
      bool normalized = false;

      if (!sourceExists)
      {
        launchSpec.sourceListId = kAllTracksListId;
        launchSpec.quickFilterExpression.clear();
        normalized = true;
      }

      auto currentTrackId = loaded.currentTrackId;
      auto positionMs = loaded.positionMs;
      auto candidate = _succession.preparePlaybackSessionRestore(launchSpec,
                                                                 currentTrackId,
                                                                 static_cast<std::size_t>(loaded.anchorIndex),
                                                                 loaded.shuffleMode,
                                                                 loaded.repeatMode);

      if (!candidate)
      {
        return std::unexpected{candidate.error()};
      }

      if (!currentExists)
      {
        auto optReplacementIndex = std::optional<std::size_t>{};
        auto const savedAnchor = static_cast<std::size_t>(loaded.anchorIndex);

        if (savedAnchor < (*candidate)->projectionSize())
        {
          optReplacementIndex = savedAnchor;
        }
        else if (loaded.repeatMode == RepeatMode::All && (*candidate)->projectionSize() != 0)
        {
          optReplacementIndex = 0;
        }

        if (!optReplacementIndex)
        {
          return PlaybackSessionPersistenceRestoreResult{};
        }

        currentTrackId = (*candidate)->trackIdAt(*optReplacementIndex);
        positionMs = 0;
        normalized = true;
        candidate = _succession.preparePlaybackSessionRestore(
          launchSpec, currentTrackId, *optReplacementIndex, loaded.shuffleMode, loaded.repeatMode);

        if (!candidate)
        {
          return std::unexpected{candidate.error()};
        }
      }

      auto const actualAnchor = (*candidate)->cursor().anchor().anchorIndex();
      restoredState.sourceListId = launchSpec.sourceListId;
      restoredState.quickFilterExpression = launchSpec.quickFilterExpression;
      restoredState.currentTrackId = currentTrackId;
      restoredState.anchorIndex = actualAnchor;
      restoredState.positionMs = positionMs;
      normalized = normalized || restoredState != loaded;
      auto const outcome = PlaybackSessionPersistenceRestoreResult{
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

      auto restored = [&] -> Result<>
      {
        auto const restoring = RestoreIntentTransaction{_restoring};
        return _playbackTransport.restorePlaybackTransport(
          transport,
          [this,
           sessionPtr = std::move(*candidate),
           shuffleMode = loaded.shuffleMode,
           repeatMode = loaded.repeatMode,
           currentTrackId,
           sourceListId = launchSpec.sourceListId,
           &restoredState,
           &loaded,
           &normalized](std::chrono::milliseconds const elapsed) mutable noexcept
          {
            _succession.commitPlaybackSessionRestore(std::move(sessionPtr), shuffleMode, repeatMode, elapsed);

            auto const& successionState = _succession.state();
            auto const& playbackState = _playbackTransport.state();
            restoredState.positionMs =
              static_cast<std::uint64_t>(std::max(elapsed, std::chrono::milliseconds{0}).count());
            auto const coherentCurrent = successionState.currentTrackId == currentTrackId &&
                                         successionState.sourceListId == sourceListId &&
                                         playbackState.nowPlaying.trackId == currentTrackId &&
                                         playbackState.nowPlaying.sourceListId == sourceListId;
            normalized = normalized || !coherentCurrent || restoredState != loaded;
            _sessionDiscarded = false;
            _intentPosition = elapsed;
            _volumeIntent = playbackState.volume.level;
            _mutedIntent = playbackState.volume.muted;
            _lastSnapshot = _playback.snapshot();
            _sessionRevision.resetClean();

            if (normalized && _sessionRevision.markDirty())
            {
              publishDirtySafely(_dirtySignal);
            }
          });
      }();

      if (!restored)
      {
        return std::unexpected{restored.error()};
      }

      return outcome;
    }
    catch (std::exception const& error)
    {
      return std::unexpected{makeError(Error::Code::Generic, error.what())};
    }
  }

  Result<> PlaybackSessionPersistence::discardRestorableSession()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is in progress");
    }

    cancelScheduledSave();
    _nextRetryDelay = kInitialRetryDelay;

    if (auto const removed = _config.removeGroup(kPlaybackSessionConfigGroup); !removed)
    {
      return std::unexpected{removed.error()};
    }

    _succession.discardPlaybackSessionSnapshot();
    _playbackTransport.discardPlaybackTransportSnapshot();
    _sessionDiscarded = true;
    _sessionRevision.resetClean();
    return {};
  }

  async::Subscription PlaybackSessionPersistence::onDirty(std::move_only_function<void()> handler)
  {
    if (!handler)
    {
      return {};
    }

    if (_sessionRevision.dirty())
    {
      handler();
    }

    return _dirtySignal.connect(std::move(handler));
  }
} // namespace ao::rt
