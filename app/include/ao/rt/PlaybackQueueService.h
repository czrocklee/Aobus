// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "Subscription.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  class NotificationService;
  class AppRuntime;
  class PlaybackService;

  struct PlaybackQueueState final
  {
    std::vector<TrackId> trackIds{};
    std::optional<std::size_t> optCurrentIndex{};
    std::optional<std::size_t> optPendingNextIndex{};
    ListId sourceListId{kInvalidListId};
    std::uint64_t revision = 0;
  };

  /**
   * Executor-affine application queue and playback-session policy.
   *
   * Commands update state() before publishing onChanged() inline. No-op
   * commands do not increment the revision or publish an event. Subscription
   * creation and reset follow the same executor-affinity contract.
   */
  class PlaybackQueueService final
  {
  public:
    PlaybackQueueService(async::Executor& executor, PlaybackService& playback, NotificationService& notifications);
    ~PlaybackQueueService();

    PlaybackQueueService(PlaybackQueueService const&) = delete;
    PlaybackQueueService& operator=(PlaybackQueueService const&) = delete;
    PlaybackQueueService(PlaybackQueueService&&) = delete;
    PlaybackQueueService& operator=(PlaybackQueueService&&) = delete;

    PlaybackQueueState const& state() const;
    Result<> playQueue(std::vector<TrackId> trackIds, TrackId startTrackId, ListId sourceListId);

    bool hasNext() const;
    bool hasPrevious() const;
    void next();
    void previous();
    void resume();
    void clear();

    Subscription onChanged(std::move_only_function<void(PlaybackQueueState const&)> handler);

  private:
    friend class AppRuntime;

    PlaybackQueueState playbackSessionQueueState() const;
    void beginPlaybackSessionRestore(std::vector<TrackId> trackIds, std::size_t currentIndex, ListId sourceListId);
    void finishPlaybackSessionRestore();
    void cancelPlaybackSessionRestore();

    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
