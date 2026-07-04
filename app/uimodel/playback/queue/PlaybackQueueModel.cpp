// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Transport.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kRestartThreshold = std::chrono::seconds{3};

    constexpr std::size_t kMaxConsecutivePlaybackFailures = 3;

    bool isSkipEligible(rt::PlaybackFailureKind kind) noexcept
    {
      return kind == rt::PlaybackFailureKind::TrackOpen || kind == rt::PlaybackFailureKind::Decode;
    }

    bool canContinueAfterPlayFailure(Error const& error) noexcept
    {
      return error.code == Error::Code::NotFound;
    }

    std::string skippedTracksMessage(std::size_t count)
    {
      return count == 1 ? std::string{"Skipped 1 unplayable track"}
                        : "Skipped " + std::to_string(count) + " unplayable tracks";
    }

    std::string playbackStoppedMessage()
    {
      return "Playback stopped after " + std::to_string(kMaxConsecutivePlaybackFailures) + " unplayable tracks";
    }
  } // namespace

  PlaybackQueueModel::PlaybackQueueModel(rt::PlaybackService& playback, rt::NotificationService& notifications)
    : _playback{playback}, _notifications{notifications}
  {
  }

  PlaybackQueueModel::~PlaybackQueueModel()
  {
    unsubscribeEvents();
  }

  bool PlaybackQueueModel::playQueue(std::vector<TrackId> trackIds, TrackId startTrackId, ListId sourceListId)
  {
    if (trackIds.empty())
    {
      return false;
    }

    auto const it = std::ranges::find(trackIds, startTrackId);

    if (it == trackIds.end())
    {
      return false;
    }

    auto const startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));

    if (!_playback.playTrack(startTrackId, sourceListId))
    {
      return false;
    }

    unsubscribeEvents();

    _queueStatePtr = std::make_unique<PlaybackQueueState>(PlaybackQueueState{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .optPendingNextIndex = std::nullopt,
      .sourceListId = sourceListId,
    });
    _skipNotificationId = rt::kInvalidNotificationId;
    _skippedFailureCount = 0;
    _consecutivePlaybackFailures = 0;

    subscribeEvents();
    prepareNext();
    return true;
  }

  bool PlaybackQueueModel::hasNext() const
  {
    if (!_queueStatePtr)
    {
      return false;
    }

    auto const& state = _playback.state();

    if (state.repeatMode == rt::RepeatMode::One || state.repeatMode == rt::RepeatMode::All)
    {
      return !_queueStatePtr->trackIds.empty();
    }

    if (state.shuffleMode == rt::ShuffleMode::On)
    {
      return _queueStatePtr->trackIds.size() > 1;
    }

    return _queueStatePtr->currentIndex + 1 < _queueStatePtr->trackIds.size();
  }

  bool PlaybackQueueModel::hasPrevious() const
  {
    if (!_queueStatePtr)
    {
      return false;
    }

    auto const& state = _playback.state();

    if (state.elapsed > kRestartThreshold)
    {
      return true;
    }

    if (_queueStatePtr->currentIndex > 0)
    {
      return true;
    }

    if (state.repeatMode == rt::RepeatMode::All)
    {
      return !_queueStatePtr->trackIds.empty();
    }

    return false;
  }

  std::optional<TrackId> PlaybackQueueModel::peekNext()
  {
    auto const optNextIndex = peekNextIndex();

    if (!optNextIndex || !_queueStatePtr)
    {
      return std::nullopt;
    }

    return _queueStatePtr->trackIds[*optNextIndex];
  }

  void PlaybackQueueModel::next()
  {
    if (!_queueStatePtr)
    {
      return;
    }

    _consecutivePlaybackFailures = 0;
    advanceToNext();
  }

  void PlaybackQueueModel::previous()
  {
    if (!_queueStatePtr)
    {
      return;
    }

    _consecutivePlaybackFailures = 0;
    auto const& state = _playback.state();

    // If we are more than 3 seconds into the song, just restart it
    if (state.elapsed > kRestartThreshold)
    {
      auto const played = playIndex(_queueStatePtr->currentIndex);

      if (played)
      {
        return;
      }

      if (!canContinueAfterPlayFailure(played.error()))
      {
        clear();
        _playback.stop();
        return;
      }
    }

    if (_queueStatePtr->currentIndex > 0)
    {
      auto const prevIndex = _queueStatePtr->currentIndex - 1;

      for (auto [idx, trackId] :
           _queueStatePtr->trackIds | std::views::take(prevIndex + 1) | std::views::enumerate | std::views::reverse)
      {
        std::ignore = trackId;

        auto const played = playIndex(static_cast<std::size_t>(idx));

        if (played)
        {
          return;
        }

        if (!canContinueAfterPlayFailure(played.error()))
        {
          clear();
          _playback.stop();
          return;
        }
      }
    }
    else if (state.repeatMode == rt::RepeatMode::All && !_queueStatePtr->trackIds.empty())
    {
      for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate | std::views::reverse)
      {
        std::ignore = trackId;

        auto const played = playIndex(static_cast<std::size_t>(idx));

        if (played)
        {
          return;
        }

        if (!canContinueAfterPlayFailure(played.error()))
        {
          clear();
          _playback.stop();
          return;
        }
      }
    }
  }

  void PlaybackQueueModel::setShuffleMode(rt::ShuffleMode mode)
  {
    _playback.setShuffleMode(mode);

    if (_queueStatePtr)
    {
      _queueStatePtr->optPendingNextIndex.reset();
      prepareNext();
    }
  }

  void PlaybackQueueModel::setRepeatMode(rt::RepeatMode mode)
  {
    _playback.setRepeatMode(mode);

    if (_queueStatePtr)
    {
      _queueStatePtr->optPendingNextIndex.reset();
      prepareNext();
    }
  }

  void PlaybackQueueModel::resume()
  {
    _playback.resume();
  }

  bool PlaybackQueueModel::isActive() const
  {
    return _queueStatePtr != nullptr;
  }

  std::optional<TrackId> PlaybackQueueModel::nowPlayingTrackId() const
  {
    if (!_queueStatePtr || _queueStatePtr->currentIndex >= _queueStatePtr->trackIds.size())
    {
      return std::nullopt;
    }

    return _queueStatePtr->trackIds[_queueStatePtr->currentIndex];
  }

  ListId PlaybackQueueModel::sourceListId() const
  {
    if (!_queueStatePtr)
    {
      return kInvalidListId;
    }

    return _queueStatePtr->sourceListId;
  }

  void PlaybackQueueModel::clear()
  {
    _playback.clearPreparedNext();
    _queueStatePtr.reset();
    _skipNotificationId = rt::kInvalidNotificationId;
    _skippedFailureCount = 0;
    _consecutivePlaybackFailures = 0;
    unsubscribeEvents();
  }

  std::optional<std::size_t> PlaybackQueueModel::peekNextIndex()
  {
    if (!_queueStatePtr || _queueStatePtr->trackIds.empty() ||
        _queueStatePtr->currentIndex >= _queueStatePtr->trackIds.size())
    {
      return std::nullopt;
    }

    if (_queueStatePtr->optPendingNextIndex && *_queueStatePtr->optPendingNextIndex < _queueStatePtr->trackIds.size())
    {
      return _queueStatePtr->optPendingNextIndex;
    }

    auto const& state = _playback.state();
    auto optNextIndex = std::optional<std::size_t>{};

    if (state.repeatMode == rt::RepeatMode::One)
    {
      optNextIndex = _queueStatePtr->currentIndex;
    }
    else if (state.shuffleMode == rt::ShuffleMode::On && _queueStatePtr->trackIds.size() > 1)
    {
      static std::mt19937 gen(std::random_device{}());
      auto dist = std::uniform_int_distribution<std::size_t>{0, _queueStatePtr->trackIds.size() - 1};
      auto nextIndex = dist(gen);

      if (nextIndex == _queueStatePtr->currentIndex)
      {
        nextIndex = (nextIndex + 1) % _queueStatePtr->trackIds.size();
      }

      optNextIndex = nextIndex;
    }
    else if (_queueStatePtr->currentIndex + 1 < _queueStatePtr->trackIds.size())
    {
      optNextIndex = _queueStatePtr->currentIndex + 1;
    }
    else if (state.repeatMode == rt::RepeatMode::All)
    {
      optNextIndex = 0;
    }

    if (optNextIndex)
    {
      _queueStatePtr->optPendingNextIndex = optNextIndex;
    }

    return optNextIndex;
  }

  Result<> PlaybackQueueModel::playIndex(std::size_t index)
  {
    if (!_queueStatePtr || index >= _queueStatePtr->trackIds.size())
    {
      return makeError(Error::Code::NotFound, "queue index not found");
    }

    _ignoreNowPlayingChange = true;
    auto played = _playback.playTrack(_queueStatePtr->trackIds[index], _queueStatePtr->sourceListId);
    _ignoreNowPlayingChange = false;

    if (!played)
    {
      return played;
    }

    _queueStatePtr->currentIndex = index;
    _queueStatePtr->optPendingNextIndex.reset();
    prepareNext();
    return {};
  }

  bool PlaybackQueueModel::tryAdvanceToIndex(std::size_t index)
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

    clear();
    _playback.stop();
    return true;
  }

  void PlaybackQueueModel::prepareNext()
  {
    if (!_queueStatePtr)
    {
      _playback.clearPreparedNext();
      return;
    }

    auto const optNextIndex = peekNextIndex();

    if (!optNextIndex)
    {
      _playback.clearPreparedNext();
      return;
    }

    if (!_playback.prepareNext(_queueStatePtr->trackIds[*optNextIndex], _queueStatePtr->sourceListId))
    {
      _queueStatePtr->optPendingNextIndex.reset();
      _playback.clearPreparedNext();
    }
  }

  void PlaybackQueueModel::commitNowPlaying(TrackId trackId, ListId sourceListId)
  {
    if (_ignoreNowPlayingChange || !_queueStatePtr)
    {
      return;
    }

    if (_queueStatePtr->sourceListId != sourceListId)
    {
      return;
    }

    if (_queueStatePtr->optPendingNextIndex && *_queueStatePtr->optPendingNextIndex < _queueStatePtr->trackIds.size() &&
        _queueStatePtr->trackIds[*_queueStatePtr->optPendingNextIndex] == trackId)
    {
      _queueStatePtr->currentIndex = *_queueStatePtr->optPendingNextIndex;
      _queueStatePtr->optPendingNextIndex.reset();
      _consecutivePlaybackFailures = 0;
      prepareNext();
      return;
    }

    auto const it = std::ranges::find(_queueStatePtr->trackIds, trackId);

    if (it == _queueStatePtr->trackIds.end())
    {
      return;
    }

    _queueStatePtr->currentIndex = static_cast<std::size_t>(std::distance(_queueStatePtr->trackIds.begin(), it));
    _queueStatePtr->optPendingNextIndex.reset();
    _consecutivePlaybackFailures = 0;
    prepareNext();
  }

  void PlaybackQueueModel::advanceToNext()
  {
    if (!_queueStatePtr)
    {
      return;
    }

    auto const& state = _playback.state();

    if (state.repeatMode == rt::RepeatMode::One)
    {
      if (tryAdvanceToIndex(_queueStatePtr->currentIndex))
      {
        return;
      }
    }

    if (_queueStatePtr->optPendingNextIndex)
    {
      auto const nextIndex = *_queueStatePtr->optPendingNextIndex;
      _queueStatePtr->optPendingNextIndex.reset();

      if (tryAdvanceToIndex(nextIndex))
      {
        return;
      }
    }

    if (state.shuffleMode == rt::ShuffleMode::On && _queueStatePtr->trackIds.size() > 1)
    {
      if (auto const optNextIndex = peekNextIndex(); optNextIndex)
      {
        if (tryAdvanceToIndex(*optNextIndex))
        {
          return;
        }
      }
    }

    auto const nextIndex = _queueStatePtr->currentIndex + 1;

    for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate | std::views::drop(nextIndex))
    {
      std::ignore = trackId;

      if (tryAdvanceToIndex(static_cast<std::size_t>(idx)))
      {
        return;
      }
    }

    if (state.repeatMode == rt::RepeatMode::All && !_queueStatePtr->trackIds.empty())
    {
      for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate)
      {
        std::ignore = trackId;

        if (tryAdvanceToIndex(static_cast<std::size_t>(idx)))
        {
          return;
        }
      }
    }

    clear();
    _playback.stop();
  }

  void PlaybackQueueModel::handlePlaybackFailure(rt::PlaybackFailure const& failure)
  {
    if (!_queueStatePtr)
    {
      return;
    }

    if (failure.sourceListId != _queueStatePtr->sourceListId)
    {
      return;
    }

    if (_queueStatePtr->currentIndex >= _queueStatePtr->trackIds.size() ||
        _queueStatePtr->trackIds[_queueStatePtr->currentIndex] != failure.trackId)
    {
      return;
    }

    if (!isSkipEligible(failure.kind))
    {
      clear();
      _playback.stop();
      return;
    }

    ++_consecutivePlaybackFailures;

    if (_consecutivePlaybackFailures >= kMaxConsecutivePlaybackFailures)
    {
      _notifications.post(rt::NotificationSeverity::Error, playbackStoppedMessage(), true);
      clear();
      _playback.stop();
      return;
    }

    ++_skippedFailureCount;
    auto const message = skippedTracksMessage(_skippedFailureCount);

    if (_skipNotificationId == rt::kInvalidNotificationId)
    {
      _skipNotificationId = _notifications.post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = message,
        .content = rt::NotificationContentState{.title = "Playback queue", .iconName = "media-skip-forward-symbolic"},
      });
    }
    else
    {
      if (!_notifications.updateMessage(_skipNotificationId, message))
      {
        _skipNotificationId = _notifications.post(rt::NotificationRequest{
          .severity = rt::NotificationSeverity::Warning,
          .message = message,
          .content = rt::NotificationContentState{.title = "Playback queue", .iconName = "media-skip-forward-symbolic"},
        });
      }
    }

    advanceToNext();
  }

  void PlaybackQueueModel::subscribeEvents()
  {
    _idleSub = _playback.onIdle(
      [this]
      {
        if (_playback.state().transport == audio::Transport::Idle)
        {
          _consecutivePlaybackFailures = 0;
          advanceToNext();
        }
      });
    _nowPlayingSub = _playback.onNowPlayingChanged([this](rt::PlaybackService::NowPlayingChanged const& event)
                                                   { commitNowPlaying(event.trackId, event.sourceListId); });
    _failureSub =
      _playback.onPlaybackFailure([this](rt::PlaybackFailure const& failure) { handlePlaybackFailure(failure); });
    _outputDeviceChangedSub = _playback.onOutputDeviceChanged([this](auto const&) { prepareNext(); });
    _seekSub = _playback.onSeekUpdate(
      [this](rt::PlaybackService::SeekUpdate const& event)
      {
        if (event.mode == rt::PlaybackService::SeekMode::Final)
        {
          prepareNext();
        }
      });
    _stoppedSub = _playback.onStopped([this] { clear(); });
  }

  void PlaybackQueueModel::unsubscribeEvents()
  {
    _idleSub.reset();
    _nowPlayingSub.reset();
    _failureSub.reset();
    _outputDeviceChangedSub.reset();
    _seekSub.reset();
    _stoppedSub.reset();
  }
} // namespace ao::uimodel
