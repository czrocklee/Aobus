// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  struct ActivePlaybackSequence final
  {
    std::vector<TrackId> trackIds;
    std::size_t currentIndex = 0;
    ListId sourceListId{kInvalidListId};
  };

  class TrackRowCache;
  class TrackViewPage;

  class PlaybackSequenceController final
  {
  public:
    PlaybackSequenceController(rt::PlaybackService& playback, TrackRowCache& dataProvider);
    ~PlaybackSequenceController();

    PlaybackSequenceController(PlaybackSequenceController const&) = delete;
    PlaybackSequenceController& operator=(PlaybackSequenceController const&) = delete;
    PlaybackSequenceController(PlaybackSequenceController&&) = delete;
    PlaybackSequenceController& operator=(PlaybackSequenceController&&) = delete;

    // Start playback from a track view page.
    // Builds the sequence from visible tracks, resolves the descriptor for
    // startTrackId, dispatches PlayTrack, and subscribes to transport events.
    bool playFromPage(TrackViewPage& page, TrackId startTrackId);

    // Transport controls
    void next();
    void previous();
    void setShuffleMode(rt::ShuffleMode mode);
    void setRepeatMode(rt::RepeatMode mode);

    // Resume playback from a paused state.
    void resume();

    // Current playback state queries (used by status bar, navigation).
    bool isActive() const;
    std::optional<TrackId> nowPlayingTrackId() const;
    ListId sourceListId() const;

  private:
    void clear();
    void advanceToNext();
    void subscribeEvents();
    void unsubscribeEvents();

    rt::PlaybackService& _playback;
    TrackRowCache& _dataProvider;
    std::unique_ptr<ActivePlaybackSequence> _sequence;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
  };
}
