// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackSequenceController.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <algorithm>
#include <ranges>

namespace ao::gtk
{
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
    auto trackIds = page.selectionController().getVisibleTrackIds();
    if (trackIds.empty())
    {
      return false;
    }

    auto const it = std::ranges::find(trackIds, startTrackId);
    auto const startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));

    auto const optDesc = _dataProvider.getPlaybackDescriptor(startTrackId);
    if (!optDesc)
    {
      return false;
    }

    unsubscribeEvents();

    _sequence = std::make_unique<ActivePlaybackSequence>(ActivePlaybackSequence{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .optSourceListId = page.getListId(),
    });

    _playback.play(*optDesc, page.getListId());

    subscribeEvents();
    return true;
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

  std::optional<ListId> PlaybackSequenceController::sourceListId() const
  {
    if (!_sequence)
    {
      return std::nullopt;
    }

    return _sequence->optSourceListId;
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

    auto const nextIndex = _sequence->currentIndex + 1;
    for (auto i = nextIndex; i < _sequence->trackIds.size(); ++i)
    {
      auto const optDesc = _dataProvider.getPlaybackDescriptor(_sequence->trackIds[i]);
      if (optDesc)
      {
        _sequence->currentIndex = i;

        _playback.play(*optDesc, _sequence->optSourceListId.value_or(ListId{}));
        return;
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
}
