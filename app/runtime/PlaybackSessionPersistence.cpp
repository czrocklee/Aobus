// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionPersistence.h"

#include "runtime/PlaybackSessionState.h"
#include "runtime/playback/PlaybackCursorSession.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackLaunchContext.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/Signal.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

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

    void publishDirtySafely(Signal<>& signal) noexcept
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

    Result<> validatePlaybackSession(PlaybackSessionState const& session, library::MusicLibrary& storage)
    {
      if (session.schemaVersion != kPlaybackSessionSchemaVersion)
      {
        return makeError(Error::Code::FormatRejected, "Unsupported playback session schema version");
      }

      if (session.sourceListId == kInvalidListId || session.currentTrackId == kInvalidTrackId)
      {
        return makeError(Error::Code::CorruptData, "Playback session contains an invalid source or current track id");
      }

      if (session.anchorIndex > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
          session.positionMs > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()))
      {
        return makeError(Error::Code::CorruptData, "Playback session anchor or position is out of range");
      }

      if (!std::isfinite(session.volume) || session.volume < 0.0F || session.volume > 1.0F ||
          !isValidShuffleMode(session.shuffleMode) || !isValidRepeatMode(session.repeatMode))
      {
        return makeError(Error::Code::CorruptData, "Playback session contains invalid playback values");
      }

      if (session.sortBy.size() > kPlaybackSessionMaxSortTerms)
      {
        return makeError(Error::Code::CorruptData, "Playback session contains too many sort terms");
      }

      auto seenFields = std::vector<TrackSortField>{};
      seenFields.reserve(session.sortBy.size());

      for (auto const& term : session.sortBy)
      {
        if (static_cast<std::size_t>(term.field) >= kTrackSortFieldCount ||
            std::ranges::contains(seenFields, term.field))
        {
          return makeError(Error::Code::CorruptData, "Playback session contains invalid or duplicate sort fields");
        }

        seenFields.push_back(term.field);
      }

      auto parsed = query::parse(session.quickFilterExpression.empty() ? "true" : session.quickFilterExpression);

      if (!parsed)
      {
        return std::unexpected{parsed.error()};
      }

      if (auto compiled = query::compileQuery(*parsed, &storage.dictionary()); !compiled)
      {
        return std::unexpected{compiled.error()};
      }

      return {};
    }

    PlaybackSessionState snapshotState(PlaybackLaunchContext launchContext,
                                       TrackId const currentTrackId,
                                       std::size_t const anchorIndex,
                                       PlaybackTransportSessionState const& transport,
                                       PlaybackSequenceState const& sequence)
    {
      return PlaybackSessionState{
        .sourceListId = launchContext.sourceListId,
        .quickFilterExpression = std::move(launchContext.quickFilterExpression),
        .sortBy = std::move(launchContext.order.sortBy),
        .currentTrackId = currentTrackId,
        .anchorIndex = static_cast<std::uint64_t>(anchorIndex),
        .positionMs = transport.positionMs,
        .shuffleMode = sequence.shuffle,
        .repeatMode = sequence.repeat,
        .volume = transport.volume,
        .muted = transport.muted,
      };
    }
  } // namespace

  PlaybackSessionPersistence::PlaybackSessionPersistence(ConfigStore& config,
                                                         Library& library,
                                                         library::MusicLibrary& storage,
                                                         PlaybackSequenceService& sequence,
                                                         PlaybackService& playback,
                                                         async::Runtime& asyncRuntime)
    : _config{config}
    , _library{library}
    , _storage{storage}
    , _sequence{sequence}
    , _playback{playback}
    , _asyncRuntime{asyncRuntime}
    , _intentPosition{_playback.elapsed()}
    , _volumeIntent{_playback.state().volume.level}
    , _mutedIntent{_playback.state().volume.muted}
  {
    _sequenceIntentSubscription = _sequence.onPersistenceIntentChanged(
      [this]
      {
        _intentPosition = _playback.elapsed();
        markDirty();
      });
    _volumeSubscription = _playback.onVolumeChanged(
      [this](float const volume)
      {
        if (volume == _volumeIntent)
        {
          return;
        }

        _volumeIntent = volume;
        markDirty();
      });
    _mutedSubscription = _playback.onMutedChanged(
      [this](bool const muted)
      {
        if (muted == _mutedIntent)
        {
          return;
        }

        _mutedIntent = muted;
        markDirty();
      });
    _seekSubscription = _playback.onSeekUpdate(
      [this](PlaybackService::SeekUpdate const& event)
      {
        if (event.mode == PlaybackService::SeekMode::Final)
        {
          if (event.elapsed != _intentPosition)
          {
            _intentPosition = event.elapsed;
            markDirty();
          }

          if (_started)
          {
            std::ignore = checkpoint();
          }
        }
      });
  }

  PlaybackSessionPersistence::~PlaybackSessionPersistence() = default;

  void PlaybackSessionPersistence::start()
  {
    if (_started || _shuttingDown)
    {
      return;
    }

    _started = true;
    auto const weakSelfPtr = weak_from_this();
    _pausedSubscription = _playback.onPaused(
      [weakSelfPtr]
      {
        if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
        {
          std::ignore = selfPtr->checkpoint();
        }
      });
    _stoppedSubscription = _playback.onStopped(
      [weakSelfPtr]
      {
        if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
        {
          std::ignore = selfPtr->checkpoint();
        }
      });
    _nowPlayingSubscription = _playback.onNowPlayingChanged(
      [weakSelfPtr](PlaybackService::NowPlayingChanged const&)
      {
        if (auto const selfPtr = weakSelfPtr.lock(); selfPtr)
        {
          std::ignore = selfPtr->checkpoint();
        }
      });

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
    _sequenceIntentSubscription.reset();
    _volumeSubscription.reset();
    _mutedSubscription.reset();
    _seekSubscription.reset();
    _pausedSubscription.reset();
    _stoppedSubscription.reset();
    _nowPlayingSubscription.reset();
    cancelScheduledSave();
    _periodicTask.reset();
    return shouldSave ? save() : Result<>{};
  }

  bool PlaybackSessionPersistence::hasActiveSession() const
  {
    return _sequence.hasActivePlaybackSession();
  }

  bool PlaybackSessionPersistence::hasRestorableSession() const
  {
    auto context = PlaybackLaunchContext{};
    auto trackId = kInvalidTrackId;
    std::size_t anchorIndex = 0;
    return _sequence.capturePlaybackSessionSnapshot(context, trackId, anchorIndex);
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

        if (selfPtr->_playback.state().transport == audio::Transport::Playing)
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

    auto launchContext = PlaybackLaunchContext{};
    auto currentTrackId = kInvalidTrackId;
    std::size_t anchorIndex = 0;

    if (!_sequence.capturePlaybackSessionSnapshot(launchContext, currentTrackId, anchorIndex))
    {
      return {};
    }

    auto const transport = _playback.playbackTransportSessionState();

    if (transport.trackId == kInvalidTrackId || transport.trackId != currentTrackId)
    {
      return makeError(Error::Code::InvalidState, "Playback cursor and transport current tracks disagree during save");
    }

    auto const capturedRevision = _sessionRevision.capture();
    auto const session =
      snapshotState(std::move(launchContext), currentTrackId, anchorIndex, transport, _sequence.state());

    if (auto const saved = _config.saveResult(kPlaybackSessionConfigGroup, session); !saved)
    {
      return saved;
    }

    if (auto const flushed = _config.flush(); !flushed)
    {
      return flushed;
    }

    _sessionRevision.acknowledge(capturedRevision);

    return {};
  }

  Result<PlaybackSessionRestoreOutcome> PlaybackSessionPersistence::restore()
  {
    if (_restoring)
    {
      return makeError(Error::Code::InvalidState, "Playback session restore is already in progress");
    }

    auto const containsSession = _config.contains(kPlaybackSessionConfigGroup);

    if (!containsSession)
    {
      return std::unexpected{containsSession.error()};
    }

    if (!*containsSession)
    {
      return PlaybackSessionRestoreOutcome{};
    }

    auto loaded = PlaybackSessionState{};

    if (auto const result = _config.loadExact(kPlaybackSessionConfigGroup, loaded); !result)
    {
      return std::unexpected{result.error()};
    }

    if (auto const valid = validatePlaybackSession(loaded, _storage); !valid)
    {
      return std::unexpected{valid.error()};
    }

    try
    {
      auto reader = _library.reader();
      auto const currentExists = reader.containsTrack(loaded.currentTrackId);
      auto const sourceExists = loaded.sourceListId == kAllTracksListId || reader.listNode(loaded.sourceListId);

      if (!sourceExists && !currentExists)
      {
        return PlaybackSessionRestoreOutcome{};
      }

      auto restoredState = loaded;
      auto context = PlaybackLaunchContext{
        .sourceListId = loaded.sourceListId,
        .quickFilterExpression = loaded.quickFilterExpression,
        .order = TrackOrderSpec{.sortBy = loaded.sortBy},
      };
      bool normalized = false;

      if (!sourceExists)
      {
        context.sourceListId = kAllTracksListId;
        context.quickFilterExpression.clear();
        normalized = true;
      }

      auto currentTrackId = loaded.currentTrackId;
      auto positionMs = loaded.positionMs;
      auto candidate = _sequence.preparePlaybackSessionRestore(
        context, currentTrackId, static_cast<std::size_t>(loaded.anchorIndex), loaded.shuffleMode, loaded.repeatMode);

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
          return PlaybackSessionRestoreOutcome{};
        }

        currentTrackId = (*candidate)->trackIdAt(*optReplacementIndex);
        positionMs = 0;
        normalized = true;
        candidate = _sequence.preparePlaybackSessionRestore(
          context, currentTrackId, *optReplacementIndex, loaded.shuffleMode, loaded.repeatMode);

        if (!candidate)
        {
          return std::unexpected{candidate.error()};
        }
      }

      auto const actualAnchor = (*candidate)->cursor().anchor().anchorIndex();
      restoredState.sourceListId = context.sourceListId;
      restoredState.quickFilterExpression = context.quickFilterExpression;
      restoredState.currentTrackId = currentTrackId;
      restoredState.anchorIndex = actualAnchor;
      restoredState.positionMs = positionMs;
      normalized = normalized || restoredState != loaded;
      auto const outcome = PlaybackSessionRestoreOutcome{
        .restored = true,
        .trackId = currentTrackId,
        .sourceListId = context.sourceListId,
      };
      auto const transport = PlaybackTransportSessionState{
        .sourceListId = context.sourceListId,
        .trackId = currentTrackId,
        .positionMs = positionMs,
        .volume = loaded.volume,
        .muted = loaded.muted,
      };

      auto restored = [&] -> Result<>
      {
        auto const restoring = RestoreIntentTransaction{_restoring};
        return _playback.restorePlaybackTransport(
          transport,
          [this,
           sessionPtr = std::move(*candidate),
           shuffleMode = loaded.shuffleMode,
           repeatMode = loaded.repeatMode,
           currentTrackId,
           sourceListId = context.sourceListId,
           &restoredState,
           &loaded,
           &normalized](std::chrono::milliseconds const elapsed) mutable noexcept
          {
            _sequence.commitPlaybackSessionRestore(std::move(sessionPtr), shuffleMode, repeatMode, elapsed);

            auto const& sequenceState = _sequence.state();
            auto const& playbackState = _playback.state();
            restoredState.positionMs =
              static_cast<std::uint64_t>(std::max(elapsed, std::chrono::milliseconds{0}).count());
            auto const coherentCurrent = sequenceState.currentTrackId == currentTrackId &&
                                         sequenceState.sourceListId == sourceListId &&
                                         playbackState.nowPlaying.trackId == currentTrackId &&
                                         playbackState.nowPlaying.sourceListId == sourceListId;
            normalized = normalized || !coherentCurrent || restoredState != loaded;
            _sessionDiscarded = false;
            _intentPosition = elapsed;
            _volumeIntent = playbackState.volume.level;
            _mutedIntent = playbackState.volume.muted;
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

    if (auto const flushed = _config.flush(); !flushed)
    {
      return flushed;
    }

    _sequence.discardPlaybackSessionSnapshot();
    _playback.discardPlaybackTransportSnapshot();
    _sessionDiscarded = true;
    _sessionRevision.resetClean();
    return {};
  }

  Subscription PlaybackSessionPersistence::onDirty(std::move_only_function<void()> handler)
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
