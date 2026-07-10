// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/audio/Transport.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <source_location>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr auto kRestartThreshold = std::chrono::seconds{3};
    constexpr std::size_t kMaxConsecutivePlaybackFailures = 3;

    bool isSkipEligible(PlaybackFailureKind const kind) noexcept
    {
      return kind == PlaybackFailureKind::TrackOpen || kind == PlaybackFailureKind::Decode;
    }

    bool canContinueAfterPlayFailure(Error const& error) noexcept
    {
      return error.code == Error::Code::NotFound;
    }

    std::string skippedTracksMessage(std::size_t const count)
    {
      return count == 1 ? std::string{"Skipped 1 unplayable track"}
                        : "Skipped " + std::to_string(count) + " unplayable tracks";
    }

    std::string playbackStoppedMessage()
    {
      return "Playback stopped after " + std::to_string(kMaxConsecutivePlaybackFailures) + " unplayable tracks";
    }

    [[noreturn]] void failExecutorAffinity(std::source_location const& loc)
    {
      APP_LOG_CRITICAL("PlaybackQueueService thread-affinity violation: '{}' invoked off the executor thread ({}:{})",
                       loc.function_name(),
                       loc.file_name(),
                       loc.line());

      if (auto const& loggerPtr = Log::appLogger(); loggerPtr)
      {
        loggerPtr->flush();
      }

      std::abort();
    }
  } // namespace

  struct PlaybackQueueService::Impl final
  {
    async::Executor& executor;
    PlaybackService& playback;
    NotificationService& notifications;
    PlaybackQueueState state{};
    std::optional<PlaybackQueueState> optLastRestorableState{};
    std::optional<PlaybackQueueState> optPreRestoreState{};
    Signal<PlaybackQueueState const&> changedSignal;
    Subscription idleSub;
    Subscription nowPlayingSub;
    Subscription outputDeviceChangedSub;
    Subscription seekSub;
    Subscription stoppedSub;
    Subscription shuffleModeSub;
    Subscription repeatModeSub;
    NotificationId skipNotificationId = kInvalidNotificationId;
    std::size_t skippedFailureCount = 0;
    std::size_t consecutivePlaybackFailures = 0;
    std::atomic_bool closing{false};
    bool ignoreNowPlayingChange = false;
    bool restoringPlaybackSession = false;

    Impl(async::Executor& executor, PlaybackService& playback, NotificationService& notifications)
      : executor{executor}, playback{playback}, notifications{notifications}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    bool isClosing() const noexcept { return closing.load(std::memory_order_acquire); }

    void ensureOnExecutor(std::source_location loc = std::source_location::current()) const
    {
      if (!executor.isCurrent()) [[unlikely]]
      {
        failExecutorAffinity(loc);
      }
    }

    bool isActive() const noexcept
    {
      return state.optCurrentIndex.value_or(state.trackIds.size()) < state.trackIds.size();
    }

    void publishChanged()
    {
      if (isClosing())
      {
        return;
      }

      ++state.revision;
      changedSignal.emit(state);
    }

    void resetFailureState() noexcept
    {
      skipNotificationId = kInvalidNotificationId;
      skippedFailureCount = 0;
      consecutivePlaybackFailures = 0;
    }

    bool clearState()
    {
      auto const changed = !state.trackIds.empty() || state.optCurrentIndex || state.optPendingNextIndex ||
                           state.sourceListId != kInvalidListId;
      playback.clearPreparedNext();

      if (!changed)
      {
        return false;
      }

      optLastRestorableState = state;
      auto const revision = state.revision;
      state = PlaybackQueueState{.revision = revision};
      resetFailureState();
      publishChanged();
      return true;
    }

    std::optional<std::size_t> peekNextIndex()
    {
      if (!isActive())
      {
        return std::nullopt;
      }

      if (state.optPendingNextIndex && *state.optPendingNextIndex < state.trackIds.size())
      {
        return state.optPendingNextIndex;
      }

      auto const& playbackState = playback.state();
      auto optNextIndex = std::optional<std::size_t>{};
      auto const currentIndex = state.optCurrentIndex.value_or(state.trackIds.size());

      if (playbackState.mode.repeat == RepeatMode::One)
      {
        optNextIndex = currentIndex;
      }
      else if (playbackState.mode.shuffle == ShuffleMode::On && state.trackIds.size() > 1)
      {
        static auto generator = std::mt19937{std::random_device{}()};
        auto distribution = std::uniform_int_distribution<std::size_t>{0, state.trackIds.size() - 1};
        auto nextIndex = distribution(generator);

        if (nextIndex == currentIndex)
        {
          nextIndex = (nextIndex + 1) % state.trackIds.size();
        }

        optNextIndex = nextIndex;
      }
      else if (currentIndex + 1 < state.trackIds.size())
      {
        optNextIndex = currentIndex + 1;
      }
      else if (playbackState.mode.repeat == RepeatMode::All)
      {
        optNextIndex = 0;
      }

      state.optPendingNextIndex = optNextIndex;
      return optNextIndex;
    }

    void prepareNext()
    {
      if (!isActive())
      {
        playback.clearPreparedNext();
        return;
      }

      auto const optNextIndex = peekNextIndex();

      if (!optNextIndex)
      {
        playback.clearPreparedNext();
        return;
      }

      if (!playback.prepareNext(state.trackIds[*optNextIndex], state.sourceListId))
      {
        state.optPendingNextIndex.reset();
        playback.clearPreparedNext();
      }
    }

    Result<> playIndex(std::size_t const index)
    {
      if (!isActive() || index >= state.trackIds.size())
      {
        return makeError(Error::Code::NotFound, "queue index not found");
      }

      auto const optPreviousIndex = state.optCurrentIndex;
      auto const optPreviousPendingIndex = state.optPendingNextIndex;
      ignoreNowPlayingChange = true;
      auto played = playback.playTrack(state.trackIds[index], state.sourceListId);
      ignoreNowPlayingChange = false;

      if (!played)
      {
        return played;
      }

      state.optCurrentIndex = index;
      state.optPendingNextIndex.reset();
      prepareNext();

      if (state.optCurrentIndex != optPreviousIndex || state.optPendingNextIndex != optPreviousPendingIndex)
      {
        publishChanged();
      }

      return {};
    }

    bool tryAdvanceToIndex(std::size_t const index)
    {
      auto const played = playIndex(index);

      if (played)
      {
        return true;
      }

      if (canContinueAfterPlayFailure(played.error()))
      {
        return false;
      }

      clearState();
      playback.stop();
      return true;
    }

    void advanceToNext()
    {
      if (!isActive())
      {
        return;
      }

      auto const& playbackState = playback.state();
      auto const currentIndex = state.optCurrentIndex.value_or(state.trackIds.size());

      if (playbackState.mode.repeat == RepeatMode::One && tryAdvanceToIndex(currentIndex))
      {
        return;
      }

      if (state.optPendingNextIndex)
      {
        auto const nextIndex = *state.optPendingNextIndex;
        state.optPendingNextIndex.reset();

        if (tryAdvanceToIndex(nextIndex))
        {
          return;
        }
      }

      if (playbackState.mode.shuffle == ShuffleMode::On && state.trackIds.size() > 1)
      {
        if (auto const optNextIndex = peekNextIndex(); optNextIndex && tryAdvanceToIndex(*optNextIndex))
        {
          return;
        }
      }

      for (auto [index, trackId] : state.trackIds | std::views::enumerate | std::views::drop(currentIndex + 1))
      {
        std::ignore = trackId;

        if (tryAdvanceToIndex(static_cast<std::size_t>(index)))
        {
          return;
        }
      }

      if (playbackState.mode.repeat == RepeatMode::All)
      {
        for (auto [index, trackId] : state.trackIds | std::views::enumerate)
        {
          std::ignore = trackId;

          if (tryAdvanceToIndex(static_cast<std::size_t>(index)))
          {
            return;
          }
        }
      }

      clearState();
      playback.stop();
    }

    void commitNowPlaying(TrackId const trackId, ListId const sourceListId)
    {
      if (restoringPlaybackSession || ignoreNowPlayingChange || !isActive() || state.sourceListId != sourceListId)
      {
        return;
      }

      auto optIndex = std::optional<std::size_t>{};

      if (state.optPendingNextIndex && *state.optPendingNextIndex < state.trackIds.size() &&
          state.trackIds[*state.optPendingNextIndex] == trackId)
      {
        optIndex = state.optPendingNextIndex;
      }
      else if (auto const it = std::ranges::find(state.trackIds, trackId); it != state.trackIds.end())
      {
        optIndex = static_cast<std::size_t>(std::distance(state.trackIds.begin(), it));
      }

      if (!optIndex)
      {
        return;
      }

      auto const optPreviousIndex = state.optCurrentIndex;
      auto const optPreviousPendingIndex = state.optPendingNextIndex;
      state.optCurrentIndex = optIndex;
      state.optPendingNextIndex.reset();
      consecutivePlaybackFailures = 0;
      prepareNext();

      if (state.optCurrentIndex != optPreviousIndex || state.optPendingNextIndex != optPreviousPendingIndex)
      {
        publishChanged();
      }
    }

    PlaybackFailureDisposition handlePlaybackFailure(PlaybackFailure const& failure)
    {
      if (!isActive() || failure.sourceListId != state.sourceListId ||
          state.trackIds[state.optCurrentIndex.value_or(state.trackIds.size())] != failure.trackId)
      {
        return PlaybackFailureDisposition::Unhandled;
      }

      if (!isSkipEligible(failure.kind))
      {
        clearState();
        playback.stop();
        return PlaybackFailureDisposition::Stopped;
      }

      ++consecutivePlaybackFailures;

      if (consecutivePlaybackFailures >= kMaxConsecutivePlaybackFailures)
      {
        notifications.post(NotificationSeverity::Error, playbackStoppedMessage(), true);
        clearState();
        playback.stop();
        return PlaybackFailureDisposition::Stopped;
      }

      ++skippedFailureCount;
      auto const message = skippedTracksMessage(skippedFailureCount);

      if (skipNotificationId == kInvalidNotificationId || !notifications.updateMessage(skipNotificationId, message))
      {
        skipNotificationId = notifications.post(NotificationRequest{
          .severity = NotificationSeverity::Warning,
          .message = message,
          .content = NotificationContentState{.topic = NotificationTopic::PlaybackQueue},
        });
      }

      advanceToNext();
      return isActive() ? PlaybackFailureDisposition::Recovered : PlaybackFailureDisposition::Stopped;
    }

    void reprepareNext()
    {
      if (!isActive())
      {
        playback.clearPreparedNext();
        return;
      }

      auto const optPreviousPendingIndex = state.optPendingNextIndex;
      state.optPendingNextIndex.reset();
      prepareNext();

      if (state.optPendingNextIndex != optPreviousPendingIndex)
      {
        publishChanged();
      }
    }

    void start(std::weak_ptr<Impl> const& weakSelfPtr)
    {
      playback.bindPlaybackFailureRecovery(
        [weakSelfPtr](PlaybackFailure const& failure)
        {
          auto const selfPtr = weakSelfPtr.lock();

          if (!selfPtr || selfPtr->isClosing())
          {
            return PlaybackFailureDisposition::Unhandled;
          }

          return selfPtr->handlePlaybackFailure(failure);
        });

      idleSub = playback.onIdle(
        [weakSelfPtr]
        {
          auto const selfPtr = weakSelfPtr.lock();

          if (!selfPtr || selfPtr->isClosing() || selfPtr->restoringPlaybackSession)
          {
            return;
          }

          if (selfPtr->playback.state().transport == audio::Transport::Idle)
          {
            selfPtr->consecutivePlaybackFailures = 0;
            selfPtr->advanceToNext();
          }
        });
      nowPlayingSub = playback.onNowPlayingChanged(
        [weakSelfPtr](PlaybackService::NowPlayingChanged const& event)
        {
          if (auto const selfPtr = weakSelfPtr.lock();
              selfPtr && !selfPtr->isClosing() && !selfPtr->restoringPlaybackSession)
          {
            selfPtr->commitNowPlaying(event.trackId, event.sourceListId);
          }
        });
      outputDeviceChangedSub = playback.onOutputDeviceChanged(
        [weakSelfPtr](auto const&)
        {
          if (auto const selfPtr = weakSelfPtr.lock();
              selfPtr && !selfPtr->isClosing() && !selfPtr->restoringPlaybackSession)
          {
            selfPtr->prepareNext();
          }
        });
      seekSub = playback.onSeekUpdate(
        [weakSelfPtr](PlaybackService::SeekUpdate const& event)
        {
          if (event.mode == PlaybackService::SeekMode::Final)
          {
            if (auto const selfPtr = weakSelfPtr.lock();
                selfPtr && !selfPtr->isClosing() && !selfPtr->restoringPlaybackSession)
            {
              selfPtr->prepareNext();
            }
          }
        });
      stoppedSub = playback.onStopped(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock();
              selfPtr && !selfPtr->isClosing() && !selfPtr->restoringPlaybackSession)
          {
            selfPtr->clearState();
          }
        });
      auto const reprepare = [weakSelfPtr](auto const&)
      {
        if (auto const selfPtr = weakSelfPtr.lock();
            selfPtr && !selfPtr->isClosing() && !selfPtr->restoringPlaybackSession)
        {
          selfPtr->reprepareNext();
        }
      };
      shuffleModeSub = playback.onShuffleModeChanged(reprepare);
      repeatModeSub = playback.onRepeatModeChanged(reprepare);
    }

    void shutdown() noexcept
    {
      if (closing.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      playback.unbindPlaybackFailureRecovery();
      idleSub.reset();
      nowPlayingSub.reset();
      outputDeviceChangedSub.reset();
      seekSub.reset();
      stoppedSub.reset();
      shuffleModeSub.reset();
      repeatModeSub.reset();
      changedSignal.disconnectAll();
    }

    ~Impl() { shutdown(); }
  };

  PlaybackQueueService::PlaybackQueueService(async::Executor& executor,
                                             PlaybackService& playback,
                                             NotificationService& notifications)
    : _implPtr{std::make_shared<Impl>(executor, playback, notifications)}
  {
    _implPtr->start(_implPtr);
  }

  PlaybackQueueService::~PlaybackQueueService()
  {
    if (auto const implPtr = _implPtr; implPtr)
    {
      implPtr->shutdown();
    }
  }

  PlaybackQueueState const& PlaybackQueueService::state() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->state;
  }

  PlaybackQueueState PlaybackQueueService::playbackSessionQueueState() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->isActive())
    {
      return implPtr->state;
    }

    return implPtr->optLastRestorableState.value_or(PlaybackQueueState{});
  }

  void PlaybackQueueService::beginPlaybackSessionRestore(std::vector<TrackId> trackIds,
                                                         std::size_t const currentIndex,
                                                         ListId const sourceListId)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    implPtr->optPreRestoreState = implPtr->state;
    auto const revision = implPtr->state.revision;
    implPtr->state = PlaybackQueueState{
      .trackIds = std::move(trackIds),
      .optCurrentIndex = currentIndex,
      .sourceListId = sourceListId,
      .revision = revision,
    };
    implPtr->restoringPlaybackSession = true;
    implPtr->resetFailureState();
  }

  void PlaybackQueueService::finishPlaybackSessionRestore()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->restoringPlaybackSession)
    {
      return;
    }

    implPtr->restoringPlaybackSession = false;
    implPtr->optPreRestoreState.reset();
    implPtr->optLastRestorableState = implPtr->state;
    implPtr->publishChanged();
  }

  void PlaybackQueueService::cancelPlaybackSessionRestore()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->restoringPlaybackSession)
    {
      return;
    }

    implPtr->state = std::move(implPtr->optPreRestoreState).value_or(PlaybackQueueState{});
    implPtr->optPreRestoreState.reset();
    implPtr->restoringPlaybackSession = false;
  }

  Result<> PlaybackQueueService::playQueue(std::vector<TrackId> trackIds,
                                           TrackId const startTrackId,
                                           ListId const sourceListId)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (trackIds.empty())
    {
      return makeError(Error::Code::InvalidInput, "playback queue must not be empty");
    }

    auto const it = std::ranges::find(trackIds, startTrackId);

    if (it == trackIds.end())
    {
      return makeError(Error::Code::NotFound, "start track is not present in playback queue");
    }

    auto const startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));
    auto previousState = implPtr->state;
    auto const revision = previousState.revision;
    implPtr->state = PlaybackQueueState{
      .trackIds = std::move(trackIds),
      .optCurrentIndex = startIndex,
      .sourceListId = sourceListId,
      .revision = revision,
    };
    implPtr->ignoreNowPlayingChange = true;
    auto played = implPtr->playback.playTrack(startTrackId, sourceListId);
    implPtr->ignoreNowPlayingChange = false;

    if (!played)
    {
      implPtr->state = std::move(previousState);
      return played;
    }

    implPtr->resetFailureState();
    implPtr->prepareNext();
    implPtr->publishChanged();
    return {};
  }

  bool PlaybackQueueService::hasNext() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->isActive())
    {
      return false;
    }

    auto const& playbackState = implPtr->playback.state();

    if (playbackState.mode.repeat == RepeatMode::One || playbackState.mode.repeat == RepeatMode::All)
    {
      return true;
    }

    if (playbackState.mode.shuffle == ShuffleMode::On)
    {
      return implPtr->state.trackIds.size() > 1;
    }

    auto const currentIndex = implPtr->state.optCurrentIndex.value_or(implPtr->state.trackIds.size());
    return currentIndex + 1 < implPtr->state.trackIds.size();
  }

  bool PlaybackQueueService::hasPrevious() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->isActive())
    {
      return false;
    }

    auto const& playbackState = implPtr->playback.state();

    if (playbackState.elapsed > kRestartThreshold ||
        implPtr->state.optCurrentIndex.value_or(implPtr->state.trackIds.size()) > 0)
    {
      return true;
    }

    return playbackState.mode.repeat == RepeatMode::All;
  }

  void PlaybackQueueService::next()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->isActive())
    {
      return;
    }

    implPtr->consecutivePlaybackFailures = 0;
    implPtr->advanceToNext();
  }

  void PlaybackQueueService::previous()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!implPtr->isActive())
    {
      return;
    }

    implPtr->consecutivePlaybackFailures = 0;
    auto const& playbackState = implPtr->playback.state();
    auto const currentIndex = implPtr->state.optCurrentIndex.value_or(implPtr->state.trackIds.size());

    if (playbackState.elapsed > kRestartThreshold)
    {
      auto const played = implPtr->playIndex(currentIndex);

      if (played)
      {
        return;
      }

      if (!canContinueAfterPlayFailure(played.error()))
      {
        implPtr->clearState();
        implPtr->playback.stop();
        return;
      }
    }

    if (currentIndex > 0)
    {
      for (auto [index, trackId] :
           implPtr->state.trackIds | std::views::take(currentIndex) | std::views::enumerate | std::views::reverse)
      {
        std::ignore = trackId;

        if (implPtr->tryAdvanceToIndex(static_cast<std::size_t>(index)))
        {
          return;
        }
      }
    }
    else if (playbackState.mode.repeat == RepeatMode::All)
    {
      for (auto [index, trackId] : implPtr->state.trackIds | std::views::enumerate | std::views::reverse)
      {
        std::ignore = trackId;

        if (implPtr->tryAdvanceToIndex(static_cast<std::size_t>(index)))
        {
          return;
        }
      }
    }
  }

  void PlaybackQueueService::resume()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    implPtr->playback.resume();
  }

  void PlaybackQueueService::clear()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    implPtr->clearState();
  }

  Subscription PlaybackQueueService::onChanged(std::move_only_function<void(PlaybackQueueState const&)> handler)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->changedSignal.connect(std::move(handler));
  }
} // namespace ao::rt
