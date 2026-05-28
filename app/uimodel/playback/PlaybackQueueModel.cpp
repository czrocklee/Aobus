// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/Type.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace ao::uimodel::playback
{
  namespace
  {
    constexpr std::uint32_t kRestartThresholdMs = 3000;
  }

  PlaybackQueueModel::PlaybackQueueModel(rt::PlaybackService& playback, DescriptorProvider descriptorProvider)
    : _playback{playback}, _descriptorProvider{std::move(descriptorProvider)}
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

    auto const optDesc = _descriptorProvider(startTrackId);

    if (!optDesc)
    {
      return false;
    }

    unsubscribeEvents();

    _queueState = std::make_unique<PlaybackQueueState>(PlaybackQueueState{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .sourceListId = sourceListId,
    });

    _playback.play(*optDesc, sourceListId);

    subscribeEvents();
    return true;
  }

  void PlaybackQueueModel::next()
  {
    advanceToNext();
  }

  void PlaybackQueueModel::previous()
  {
    if (!_queueState)
    {
      return;
    }

    auto const& state = _playback.state();

    // If we are more than 3 seconds into the song, just restart it
    if (state.positionMs > kRestartThresholdMs)
    {
      auto const optDesc = _descriptorProvider(_queueState->trackIds[_queueState->currentIndex]);

      if (optDesc)
      {
        _playback.play(*optDesc, _queueState->sourceListId);
        return;
      }
    }

    if (_queueState->currentIndex > 0)
    {
      auto const prevIndex = _queueState->currentIndex - 1;

      for (std::int32_t idx = static_cast<std::int32_t>(prevIndex); idx >= 0; --idx)
      {
        auto const optDesc = _descriptorProvider(_queueState->trackIds[idx]);

        if (optDesc)
        {
          _queueState->currentIndex = static_cast<std::size_t>(idx);
          _playback.play(*optDesc, _queueState->sourceListId);
          return;
        }
      }
    }
    else if (state.repeatMode == rt::RepeatMode::All && !_queueState->trackIds.empty())
    {
      for (std::int32_t idx = static_cast<std::int32_t>(_queueState->trackIds.size() - 1); idx >= 0; --idx)
      {
        auto const optDesc = _descriptorProvider(_queueState->trackIds[idx]);

        if (optDesc)
        {
          _queueState->currentIndex = static_cast<std::size_t>(idx);
          _playback.play(*optDesc, _queueState->sourceListId);
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
    return _queueState != nullptr;
  }

  std::optional<TrackId> PlaybackQueueModel::nowPlayingTrackId() const
  {
    if (!_queueState || _queueState->currentIndex >= _queueState->trackIds.size())
    {
      return std::nullopt;
    }

    return _queueState->trackIds[_queueState->currentIndex];
  }

  ListId PlaybackQueueModel::sourceListId() const
  {
    if (!_queueState)
    {
      return kInvalidListId;
    }

    return _queueState->sourceListId;
  }

  void PlaybackQueueModel::clear()
  {
    _queueState.reset();
    unsubscribeEvents();
  }

  void PlaybackQueueModel::advanceToNext()
  {
    if (!_queueState)
    {
      return;
    }

    auto const& state = _playback.state();

    if (state.repeatMode == rt::RepeatMode::One)
    {
      auto const optDesc = _descriptorProvider(_queueState->trackIds[_queueState->currentIndex]);

      if (optDesc)
      {
        _playback.play(*optDesc, _queueState->sourceListId);
        return;
      }
    }

    if (state.shuffleMode == rt::ShuffleMode::On && _queueState->trackIds.size() > 1)
    {
      static std::mt19937 gen(std::random_device{}());
      auto dist = std::uniform_int_distribution<std::size_t>{0, _queueState->trackIds.size() - 1};

      // Simple random pick that isn't the current track
      auto nextIdx = dist(gen);

      if (nextIdx == _queueState->currentIndex)
      {
        nextIdx = (nextIdx + 1) % _queueState->trackIds.size();
      }

      auto const optDesc = _descriptorProvider(_queueState->trackIds[nextIdx]);

      if (optDesc)
      {
        _queueState->currentIndex = nextIdx;
        _playback.play(*optDesc, _queueState->sourceListId);
        return;
      }
    }

    auto const nextIndex = _queueState->currentIndex + 1;

    for (auto idx = nextIndex; idx < _queueState->trackIds.size(); ++idx)
    {
      auto const optDesc = _descriptorProvider(_queueState->trackIds[idx]);

      if (optDesc)
      {
        _queueState->currentIndex = idx;
        _playback.play(*optDesc, _queueState->sourceListId);
        return;
      }
    }

    if (state.repeatMode == rt::RepeatMode::All && !_queueState->trackIds.empty())
    {
      for (std::size_t idx = 0; idx < _queueState->trackIds.size(); ++idx)
      {
        auto const optDesc = _descriptorProvider(_queueState->trackIds[idx]);

        if (optDesc)
        {
          _queueState->currentIndex = idx;
          _playback.play(*optDesc, _queueState->sourceListId);
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
} // namespace ao::uimodel::playback
