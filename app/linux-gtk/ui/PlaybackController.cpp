// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackController.h"
#include "TrackRowDataProvider.h"
#include "TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <algorithm>
#include <ranges>

namespace ao::gtk
{
  PlaybackController::PlaybackController(ao::rt::AppSession& session, TrackRowDataProvider& dataProvider)
    : _session{session}, _dataProvider{dataProvider}
  {
  }

  PlaybackController::~PlaybackController()
  {
    unsubscribeEvents();
  }

  bool PlaybackController::playFromPage(TrackViewPage& page, ao::TrackId startTrackId)
  {
    auto trackIds = page.getVisibleTrackIds();
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

    _session.playback().play(*optDesc, page.getListId());

    subscribeEvents();
    return true;
  }

  void PlaybackController::resume()
  {
    _session.playback().resume();
  }

  bool PlaybackController::isActive() const
  {
    return _sequence != nullptr;
  }

  std::optional<ao::TrackId> PlaybackController::nowPlayingTrackId() const
  {
    if (!_sequence || _sequence->currentIndex >= _sequence->trackIds.size())
    {
      return std::nullopt;
    }

    return _sequence->trackIds[_sequence->currentIndex];
  }

  std::optional<ao::ListId> PlaybackController::sourceListId() const
  {
    if (!_sequence)
    {
      return std::nullopt;
    }

    return _sequence->optSourceListId;
  }

  void PlaybackController::clear()
  {
    _sequence.reset();
    unsubscribeEvents();
  }

  void PlaybackController::advanceToNext()
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

        _session.playback().play(*optDesc, _sequence->optSourceListId.value_or(ao::ListId{}));
        return;
      }
    }

    clear();

    _session.playback().stop();
  }

  void PlaybackController::subscribeEvents()
  {
    _idleSub = _session.playback().onIdle([this] { advanceToNext(); });

    _stoppedSub = _session.playback().onStopped([this] { clear(); });
  }

  void PlaybackController::unsubscribeEvents()
  {
    _idleSub.reset();
    _stoppedSub.reset();
  }
}
