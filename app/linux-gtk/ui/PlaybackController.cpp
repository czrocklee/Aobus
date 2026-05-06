// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackController.h"
#include "TrackRowDataProvider.h"
#include "TrackViewPage.h"

#include <runtime/AppSession.h>
#include <runtime/CommandTypes.h>
#include <runtime/EventTypes.h>

#include <algorithm>
#include <ranges>

namespace ao::gtk
{
  PlaybackController::PlaybackController(ao::app::AppSession& session, TrackRowDataProvider& dataProvider)
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

    auto desc = _dataProvider.getPlaybackDescriptor(startTrackId);
    if (!desc)
    {
      return false;
    }

    unsubscribeEvents();

    _sequence = std::unique_ptr<ActivePlaybackSequence>(new ActivePlaybackSequence{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .sourceListId = page.getListId(),
    });

    _session.commands().execute<ao::app::PlayTrack>(ao::app::PlayTrack{
      .descriptor = *desc,
      .sourceListId = page.getListId(),
    });

    subscribeEvents();
    return true;
  }

  void PlaybackController::resume()
  {
    _session.commands().execute<ao::app::ResumePlayback>(ao::app::ResumePlayback{});
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
    return _sequence->sourceListId;
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
      auto desc = _dataProvider.getPlaybackDescriptor(_sequence->trackIds[i]);
      if (desc)
      {
        _sequence->currentIndex = i;

        _session.commands().execute<ao::app::PlayTrack>(ao::app::PlayTrack{
          .descriptor = *desc,
          .sourceListId = _sequence->sourceListId.value_or(ao::ListId{}),
        });
        return;
      }
    }

    clear();

    _session.commands().execute<ao::app::StopPlayback>(ao::app::StopPlayback{});
  }

  void PlaybackController::subscribeEvents()
  {
    _transportSub = _session.events().subscribe<ao::app::PlaybackTransportChanged>(
      [this](ao::app::PlaybackTransportChanged const& event)
      {
        if (event.transport == ao::audio::Transport::Idle)
        {
          advanceToNext();
        }
      });

    _stoppedSub =
      _session.events().subscribe<ao::app::PlaybackStopped>([this](ao::app::PlaybackStopped const&) { clear(); });
  }

  void PlaybackController::unsubscribeEvents()
  {
    _transportSub.reset();
    _stoppedSub.reset();
  }
}
