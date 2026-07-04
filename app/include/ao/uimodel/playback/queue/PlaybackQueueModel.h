// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackState.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::uimodel
{
  struct PlaybackQueueState final
  {
    std::vector<TrackId> trackIds;
    std::size_t currentIndex = 0;
    std::optional<std::size_t> optPendingNextIndex;
    ListId sourceListId{kInvalidListId};
  };

  class PlaybackQueueModel final
  {
  public:
    explicit PlaybackQueueModel(rt::PlaybackService& playback);
    ~PlaybackQueueModel();

    PlaybackQueueModel(PlaybackQueueModel const&) = delete;
    PlaybackQueueModel& operator=(PlaybackQueueModel const&) = delete;
    PlaybackQueueModel(PlaybackQueueModel&&) = delete;
    PlaybackQueueModel& operator=(PlaybackQueueModel&&) = delete;

    // Start playback from a queue of tracks.
    // Builds the queue, resolves the descriptor for startTrackId,
    // dispatches PlayTrack, and subscribes to transport events.
    bool playQueue(std::vector<TrackId> trackIds, TrackId startTrackId, ListId sourceListId);

    // Transport controls
    bool hasNext() const;
    bool hasPrevious() const;
    std::optional<TrackId> peekNext();
    void next();
    void previous();
    void resume();

    void setShuffleMode(rt::ShuffleMode mode);
    void setRepeatMode(rt::RepeatMode mode);

    // Current playback state queries
    bool isActive() const;
    std::optional<TrackId> nowPlayingTrackId() const;
    ListId sourceListId() const;

  private:
    void clear();
    std::optional<std::size_t> peekNextIndex();
    bool playIndex(std::size_t index);
    void prepareNext();
    void commitNowPlaying(TrackId trackId, ListId sourceListId);
    void advanceToNext();
    void subscribeEvents();
    void unsubscribeEvents();

    rt::PlaybackService& _playback;
    std::unique_ptr<PlaybackQueueState> _queueStatePtr;
    rt::Subscription _idleSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _outputDeviceChangedSub;
    rt::Subscription _seekSub;
    rt::Subscription _stoppedSub;
    bool _ignoreNowPlayingChange = false;
  };
} // namespace ao::uimodel
