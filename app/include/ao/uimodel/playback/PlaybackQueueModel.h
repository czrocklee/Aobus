// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include <ao/audio/Types.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::uimodel::playback
{
  struct PlaybackQueueState final
  {
    std::vector<TrackId> trackIds;
    std::size_t currentIndex = 0;
    ListId sourceListId{kInvalidListId};
  };

  class PlaybackQueueModel final
  {
  public:
    using DescriptorProvider = std::function<std::optional<audio::TrackPlaybackDescriptor>(TrackId)>;

    PlaybackQueueModel(rt::PlaybackService& playback, DescriptorProvider descriptorProvider);
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
    void advanceToNext();
    void subscribeEvents();
    void unsubscribeEvents();

    rt::PlaybackService& _playback;
    DescriptorProvider _descriptorProvider;
    std::unique_ptr<PlaybackQueueState> _queueState;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
  };
} // namespace ao::uimodel::playback
