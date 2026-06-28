// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kRestartThreshold = std::chrono::seconds{3};
  }

  PlaybackQueueModel::PlaybackQueueModel(rt::PlaybackService& playback)
    : _playback{playback}
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
      .sourceListId = sourceListId,
    });

    subscribeEvents();
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

  void PlaybackQueueModel::next()
  {
    advanceToNext();
  }

  void PlaybackQueueModel::previous()
  {
    if (!_queueStatePtr)
    {
      return;
    }

    auto const& state = _playback.state();

    // If we are more than 3 seconds into the song, just restart it
    if (state.elapsed > kRestartThreshold)
    {
      if (_playback.playTrack(_queueStatePtr->trackIds[_queueStatePtr->currentIndex], _queueStatePtr->sourceListId))
      {
        return;
      }
    }

    if (_queueStatePtr->currentIndex > 0)
    {
      auto const prevIndex = _queueStatePtr->currentIndex - 1;

      for (auto [idx, trackId] :
           _queueStatePtr->trackIds | std::views::take(prevIndex + 1) | std::views::enumerate | std::views::reverse)
      {
        if (_playback.playTrack(trackId, _queueStatePtr->sourceListId))
        {
          _queueStatePtr->currentIndex = static_cast<std::size_t>(idx);
          return;
        }
      }
    }
    else if (state.repeatMode == rt::RepeatMode::All && !_queueStatePtr->trackIds.empty())
    {
      for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate | std::views::reverse)
      {
        if (_playback.playTrack(trackId, _queueStatePtr->sourceListId))
        {
          _queueStatePtr->currentIndex = static_cast<std::size_t>(idx);
          return;
        }
      }
    }
  }

  void PlaybackQueueModel::setShuffleMode(rt::ShuffleMode mode)
  {
    _playback.setShuffleMode(mode);
  }

  void PlaybackQueueModel::setRepeatMode(rt::RepeatMode mode)
  {
    _playback.setRepeatMode(mode);
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
    _queueStatePtr.reset();
    unsubscribeEvents();
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
      if (_playback.playTrack(_queueStatePtr->trackIds[_queueStatePtr->currentIndex], _queueStatePtr->sourceListId))
      {
        return;
      }
    }

    if (state.shuffleMode == rt::ShuffleMode::On && _queueStatePtr->trackIds.size() > 1)
    {
      static std::mt19937 gen(std::random_device{}());
      auto dist = std::uniform_int_distribution<std::size_t>{0, _queueStatePtr->trackIds.size() - 1};

      // Simple random pick that isn't the current track
      auto nextIdx = dist(gen);

      if (nextIdx == _queueStatePtr->currentIndex)
      {
        nextIdx = (nextIdx + 1) % _queueStatePtr->trackIds.size();
      }

      if (_playback.playTrack(_queueStatePtr->trackIds[nextIdx], _queueStatePtr->sourceListId))
      {
        _queueStatePtr->currentIndex = nextIdx;
        return;
      }
    }

    auto const nextIndex = _queueStatePtr->currentIndex + 1;

    for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate | std::views::drop(nextIndex))
    {
      if (_playback.playTrack(trackId, _queueStatePtr->sourceListId))
      {
        _queueStatePtr->currentIndex = static_cast<std::size_t>(idx);
        return;
      }
    }

    if (state.repeatMode == rt::RepeatMode::All && !_queueStatePtr->trackIds.empty())
    {
      for (auto [idx, trackId] : _queueStatePtr->trackIds | std::views::enumerate)
      {
        if (_playback.playTrack(trackId, _queueStatePtr->sourceListId))
        {
          _queueStatePtr->currentIndex = static_cast<std::size_t>(idx);
          return;
        }
      }
    }

    clear();
    _playback.stop();
  }

  void PlaybackQueueModel::subscribeEvents()
  {
    _idleSub = _playback.onIdle([this] { advanceToNext(); });
    _stoppedSub = _playback.onStopped([this] { clear(); });
  }

  void PlaybackQueueModel::unsubscribeEvents()
  {
    _idleSub.reset();
    _stoppedSub.reset();
  }
} // namespace ao::uimodel
