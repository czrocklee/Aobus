// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackActions.h"

#include "SelectionNavigation.h"
#include "TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    bool shouldPause(audio::Transport const transport)
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }
  } // namespace

  bool playSelected(rt::PlaybackQueueService& queue,
                    std::vector<TrackListEntry> const& tracks,
                    std::int32_t const selected,
                    ListId const sourceListId)
  {
    if (tracks.empty())
    {
      return false;
    }

    auto const index = clampSelection(static_cast<std::size_t>(std::max(0, selected)), tracks.size());
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(tracks.size());
    std::ranges::transform(tracks, std::back_inserter(trackIds), &TrackListEntry::id);
    return static_cast<bool>(queue.playQueue(std::move(trackIds), tracks[index].id, sourceListId));
  }

  bool togglePlayback(rt::PlaybackService& playback,
                      rt::PlaybackQueueService& queue,
                      std::vector<TrackListEntry> const& tracks,
                      std::int32_t const selected,
                      ListId const sourceListId)
  {
    auto const transport = playback.state().transport;

    if (shouldPause(transport))
    {
      playback.pause();
      return true;
    }

    if (transport == audio::Transport::Paused)
    {
      playback.resume();
      return true;
    }

    return playSelected(queue, tracks, selected, sourceListId);
  }

  void seekBy(rt::PlaybackService& playback, std::chrono::milliseconds const delta)
  {
    auto const state = playback.state();
    auto targetElapsed = state.elapsed + delta;

    if (targetElapsed < std::chrono::milliseconds{0})
    {
      targetElapsed = std::chrono::milliseconds{0};
    }

    if (state.duration.count() > 0 && targetElapsed > state.duration)
    {
      targetElapsed = state.duration;
    }

    playback.seek(targetElapsed);
  }

  void changeVolume(rt::PlaybackService& playback, float const delta)
  {
    auto const state = playback.state();
    playback.setVolume(std::clamp(state.volume.level + delta, 0.0F, 1.0F));
  }
} // namespace ao::tui
