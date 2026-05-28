// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackSequenceController.h"

#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr std::uint32_t kRestartThresholdMs = 3000;
  }

  PlaybackSequenceController::PlaybackSequenceController(rt::PlaybackService& playback, TrackRowCache& dataProvider)
    : _playback{playback}, _dataProvider{dataProvider}
  {
  }

  PlaybackSequenceController::~PlaybackSequenceController()
  {
    unsubscribeEvents();
  }

  bool PlaybackSequenceController::playFromPage(TrackViewPage& page, TrackId startTrackId)
  {
    auto trackIds = page.selectionController().visibleTrackIds();

    if (trackIds.empty())
    {
      return false;
    }

    auto const it = std::ranges::find(trackIds, startTrackId);
    auto const startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));

    auto const optDesc = _dataProvider.playbackDescriptor(startTrackId);

    if (!optDesc)
    {
      return false;
    }

    unsubscribeEvents();

    _sequence = std::make_unique<ActivePlaybackSequence>(ActivePlaybackSequence{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .sourceListId = page.listId(),
    });

    _playback.play(*optDesc, page.listId());

    subscribeEvents();
    return true;
  }

  void PlaybackSequenceController::next()
  {
    advanceToNext();
  }

  void PlaybackSequenceController::previous()
  {
    if (!_sequence)
    {
      return;
    }

    auto const& state = _playback.state();

    // If we are more than 3 seconds into the song, just restart it
    if (state.positionMs > kRestartThresholdMs)
    {
      auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[_sequence->currentIndex]);

      if (optDesc)
      {
        _playback.play(*optDesc, _sequence->sourceListId);
        return;
      }
    }

    if (_sequence->currentIndex > 0)
    {
      auto const prevIndex = _sequence->currentIndex - 1;

      for (std::int32_t idx = static_cast<std::int32_t>(prevIndex); idx >= 0; --idx)
      {
        auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[idx]);

        if (optDesc)
        {
          _sequence->currentIndex = static_cast<std::size_t>(idx);
          _playback.play(*optDesc, _sequence->sourceListId);
          return;
        }
      }
    }
    else if (state.repeatMode == rt::RepeatMode::All && !_sequence->trackIds.empty())
    {
      for (std::int32_t idx = static_cast<std::int32_t>(_sequence->trackIds.size() - 1); idx >= 0; --idx)
      {
        auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[idx]);

        if (optDesc)
        {
          _sequence->currentIndex = static_cast<std::size_t>(idx);
          _playback.play(*optDesc, _sequence->sourceListId);
          return;
        }
      }
    }
  }

  void PlaybackSequenceController::setShuffleMode(rt::ShuffleMode mode)
  {
    _playback.setShuffleMode(mode);
    // TODO: Actually shuffle the sequence if needed. For now we just pick random on advance.
  }

  void PlaybackSequenceController::setRepeatMode(rt::RepeatMode mode)
  {
    _playback.setRepeatMode(mode);
  }

  void PlaybackSequenceController::resume()
  {
    _playback.resume();
  }

  bool PlaybackSequenceController::isActive() const
  {
    return _sequence != nullptr;
  }

  std::optional<TrackId> PlaybackSequenceController::nowPlayingTrackId() const
  {
    if (!_sequence || _sequence->currentIndex >= _sequence->trackIds.size())
    {
      return std::nullopt;
    }

    return _sequence->trackIds[_sequence->currentIndex];
  }

  ListId PlaybackSequenceController::sourceListId() const
  {
    if (!_sequence)
    {
      return kInvalidListId;
    }

    return _sequence->sourceListId;
  }

  void PlaybackSequenceController::clear()
  {
    _sequence.reset();
    unsubscribeEvents();
  }

  void PlaybackSequenceController::advanceToNext()
  {
    if (!_sequence)
    {
      return;
    }

    auto const& state = _playback.state();

    if (state.repeatMode == rt::RepeatMode::One)
    {
      auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[_sequence->currentIndex]);

      if (optDesc)
      {
        _playback.play(*optDesc, _sequence->sourceListId);
        return;
      }
    }

    if (state.shuffleMode == rt::ShuffleMode::On && _sequence->trackIds.size() > 1)
    {
      static std::mt19937 gen(std::random_device{}());
      auto dist = std::uniform_int_distribution<std::size_t>{0, _sequence->trackIds.size() - 1};

      // Simple random pick that isn't the current track
      auto nextIdx = dist(gen);

      if (nextIdx == _sequence->currentIndex)
      {
        nextIdx = (nextIdx + 1) % _sequence->trackIds.size();
      }

      auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[nextIdx]);

      if (optDesc)
      {
        _sequence->currentIndex = nextIdx;
        _playback.play(*optDesc, _sequence->sourceListId);
        return;
      }
    }

    auto const nextIndex = _sequence->currentIndex + 1;

    for (auto idx = nextIndex; idx < _sequence->trackIds.size(); ++idx)
    {
      auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[idx]);

      if (optDesc)
      {
        _sequence->currentIndex = idx;
        _playback.play(*optDesc, _sequence->sourceListId);
        return;
      }
    }

    if (state.repeatMode == rt::RepeatMode::All && !_sequence->trackIds.empty())
    {
      for (std::size_t idx = 0; idx < _sequence->trackIds.size(); ++idx)
      {
        auto const optDesc = _dataProvider.playbackDescriptor(_sequence->trackIds[idx]);

        if (optDesc)
        {
          _sequence->currentIndex = idx;
          _playback.play(*optDesc, _sequence->sourceListId);
          return;
        }
      }
    }

    clear();
    _playback.stop();
  }

  void PlaybackSequenceController::subscribeEvents()
  {
    _idleSub = _playback.onIdle([this] { advanceToNext(); });
    _stoppedSub = _playback.onStopped([this] { clear(); });
  }

  void PlaybackSequenceController::unsubscribeEvents()
  {
    _idleSub.reset();
    _stoppedSub.reset();
  }
} // namespace ao::gtk
