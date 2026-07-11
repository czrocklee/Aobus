// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PlaybackMode.h"
#include "Subscription.h"
#include "ViewIds.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ao::async
{
  class Executor;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class NotificationService;
  class PlaybackCursorSession;
  class PlaybackSessionPersistence;
  class PlaybackService;
  struct PlaybackLaunchContext;
  class TrackSourceCache;
  class ViewService;

  enum class PlaybackSequenceSourceState : std::uint8_t
  {
    Inactive,
    Live,
    Invalidated,
  };

  struct PlaybackSequenceState final
  {
    PlaybackSequenceSourceState sourceState = PlaybackSequenceSourceState::Inactive;
    TrackId currentTrackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    bool hasNext = false;
    bool hasPrevious = false;
    std::optional<TrackId> optResolvedSuccessor{};
    ShuffleMode shuffle = ShuffleMode::Off;
    RepeatMode repeat = RepeatMode::Off;
    std::uint64_t semanticRevision = 0;

    bool operator==(PlaybackSequenceState const&) const = default;
  };

  /** Executor-affine live-list playback succession boundary. */
  class PlaybackSequenceService final
  {
  public:
    struct ShuffleModeChanged final
    {
      ShuffleMode mode = ShuffleMode::Off;
    };

    struct RepeatModeChanged final
    {
      RepeatMode mode = RepeatMode::Off;
    };

    PlaybackSequenceService(async::Executor& executor,
                            ViewService& views,
                            TrackSourceCache& sources,
                            library::MusicLibrary& library,
                            PlaybackService& playback,
                            NotificationService& notifications);
    ~PlaybackSequenceService();

    PlaybackSequenceService(PlaybackSequenceService const&) = delete;
    PlaybackSequenceService& operator=(PlaybackSequenceService const&) = delete;
    PlaybackSequenceService(PlaybackSequenceService&&) = delete;
    PlaybackSequenceService& operator=(PlaybackSequenceService&&) = delete;

    Result<> playFromView(ViewId viewId, TrackId startTrackId);
    bool hasNext() const;
    bool hasPrevious() const;
    void next();
    void previous();
    void clear();
    void setShuffleMode(ShuffleMode mode);
    void setRepeatMode(RepeatMode mode);

    PlaybackSequenceState const& state() const;

    Subscription onChanged(std::move_only_function<void(PlaybackSequenceState const&)> handler);
    Subscription onShuffleModeChanged(std::move_only_function<void(ShuffleModeChanged const&)> handler);
    Subscription onRepeatModeChanged(std::move_only_function<void(RepeatModeChanged const&)> handler);

  private:
    friend class PlaybackSessionPersistence;

    bool hasActivePlaybackSession() const;
    bool capturePlaybackSessionSnapshot(PlaybackLaunchContext& launchContext,
                                        TrackId& currentTrackId,
                                        std::size_t& anchorIndex) const;
    Result<std::unique_ptr<PlaybackCursorSession>> preparePlaybackSessionRestore(PlaybackLaunchContext launchContext,
                                                                                 TrackId currentTrackId,
                                                                                 std::size_t anchorIndex,
                                                                                 ShuffleMode shuffleMode,
                                                                                 RepeatMode repeatMode);
    void commitPlaybackSessionRestore(std::unique_ptr<PlaybackCursorSession> sessionPtr,
                                      ShuffleMode shuffleMode,
                                      RepeatMode repeatMode,
                                      std::chrono::milliseconds elapsed) noexcept;
    void forgetPlaybackSessionSnapshot();
    Subscription onPersistenceIntentChanged(std::move_only_function<void()> handler);

    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
