// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/Subscription.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  class NotificationService;
  class PlaybackService;
  struct PlaybackSessionState;
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

  class [[nodiscard]] PlaybackQueueSession final
  {
  public:
    PlaybackQueueSession(rt::PlaybackService& playback, rt::NotificationService& notifications);
    ~PlaybackQueueSession();

    PlaybackQueueSession(PlaybackQueueSession const&) = delete;
    PlaybackQueueSession& operator=(PlaybackQueueSession const&) = delete;
    PlaybackQueueSession(PlaybackQueueSession&&) = delete;
    PlaybackQueueSession& operator=(PlaybackQueueSession&&) = delete;

    // Start playback from a queue of tracks.
    // Builds the queue, resolves the descriptor for startTrackId,
    // dispatches PlayTrack, and subscribes to transport events.
    bool playQueue(std::vector<TrackId> trackIds, TrackId startTrackId, ListId sourceListId);
    bool restoreQueue(std::vector<TrackId> trackIds, rt::PlaybackSessionState const& session, ListId sourceListId);

    // Transport controls
    bool hasNext() const;
    bool hasPrevious() const;
    std::optional<TrackId> peekNext();
    void next();
    void previous();
    void resume();

    // Current playback state queries
    bool isActive() const;
    std::optional<TrackId> nowPlayingTrackId() const;
    ListId sourceListId() const;

  private:
    void clear();
    std::optional<std::size_t> peekNextIndex();
    Result<> playIndex(std::size_t index);
    bool tryAdvanceToIndex(std::size_t index);
    void prepareNext();
    void commitNowPlaying(TrackId trackId, ListId sourceListId);
    void handlePlaybackFailure(rt::PlaybackFailure const& failure);
    void advanceToNext();
    void subscribeEvents();
    void unsubscribeEvents();

    rt::PlaybackService& _playback;
    rt::NotificationService& _notifications;
    std::unique_ptr<PlaybackQueueState> _queueStatePtr;
    rt::Subscription _idleSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _failureSub;
    rt::Subscription _outputDeviceChangedSub;
    rt::Subscription _seekSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _shuffleModeSub;
    rt::Subscription _repeatModeSub;
    rt::NotificationId _skipNotificationId = rt::kInvalidNotificationId;
    std::size_t _skippedFailureCount = 0;
    std::size_t _consecutivePlaybackFailures = 0;
    bool _ignoreNowPlayingChange = false;
  };
} // namespace ao::uimodel
