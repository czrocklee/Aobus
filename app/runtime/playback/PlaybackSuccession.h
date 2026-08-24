// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>

namespace ao::async
{
  class Executor;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class NotificationService;
  class PlaybackCursorSession;
  class TextOrderingPolicy;
  class PlaybackSessionPersistence;
  class PlaybackTransport;
  struct PlaybackLaunchSpec;
  class TrackSourceCache;
  class ViewService;

  enum class PlaybackSuccessionSourceState : std::uint8_t
  {
    Inactive,
    Live,
    Invalidated,
  };

  struct PlaybackSuccessionState final
  {
    PlaybackSuccessionSourceState sourceState = PlaybackSuccessionSourceState::Inactive;
    TrackId currentTrackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    bool hasNext = false;
    bool hasPrevious = false;
    std::optional<TrackId> optResolvedSuccessor{};
    ShuffleMode shuffle = ShuffleMode::Off;
    RepeatMode repeat = RepeatMode::Off;

    bool operator==(PlaybackSuccessionState const&) const = default;
  };

  /** Executor-affine live-list playback succession boundary. */
  class PlaybackSuccession final
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

    PlaybackSuccession(async::Executor& executor,
                       ViewService& views,
                       TrackSourceCache& sources,
                       library::MusicLibrary const& library,
                       PlaybackTransport& transport,
                       NotificationService& notifications,
                       async::Runtime& asyncRuntime,
                       TextOrderingPolicy const* textOrderingPolicy = nullptr);
    ~PlaybackSuccession();

    PlaybackSuccession(PlaybackSuccession const&) = delete;
    PlaybackSuccession& operator=(PlaybackSuccession const&) = delete;
    PlaybackSuccession(PlaybackSuccession&&) = delete;
    PlaybackSuccession& operator=(PlaybackSuccession&&) = delete;

    Result<> playFromView(ViewId viewId, TrackId startTrackId);
    // Returns whether navigation accepted a restart, subject transition, or
    // terminal stop that establishes a new playback-clock anchor.
    bool next();
    bool previous();
    void clear();
    void setShuffleMode(ShuffleMode mode);
    void setRepeatMode(RepeatMode mode);

    PlaybackSuccessionState const& state() const;

    // Handlers run synchronously on the executor thread. Callers must defer
    // emitting-owner teardown to a later executor turn.
    async::Subscription onChanged(compat::MoveOnlyFunction<void(PlaybackSuccessionState const&)> handler);
    async::Subscription onExplicitStartSettled(compat::MoveOnlyFunction<void()> handler);
    async::Subscription onShuffleModeChanged(compat::MoveOnlyFunction<void(ShuffleModeChanged const&)> handler);
    async::Subscription onRepeatModeChanged(compat::MoveOnlyFunction<void(RepeatModeChanged const&)> handler);

  private:
    friend class PlaybackSessionPersistence;

    bool hasActivePlaybackSession() const;
    bool capturePlaybackSessionSnapshot(PlaybackLaunchSpec& launchSpec,
                                        TrackId& currentTrackId,
                                        std::size_t& anchorIndex) const;
    Result<std::unique_ptr<PlaybackCursorSession>> preparePlaybackSessionRestore(PlaybackLaunchSpec launchSpec,
                                                                                 TrackId currentTrackId,
                                                                                 std::size_t anchorIndex,
                                                                                 ShuffleMode shuffleMode,
                                                                                 RepeatMode repeatMode);
    void commitPlaybackSessionRestore(std::unique_ptr<PlaybackCursorSession> sessionPtr,
                                      ShuffleMode shuffleMode,
                                      RepeatMode repeatMode,
                                      std::chrono::milliseconds elapsed) noexcept;
    void discardPlaybackSessionSnapshot();
    async::Subscription onRestorableStateChanged(compat::MoveOnlyFunction<void()> handler);

    struct Impl;

    // Every executor-affine member starts here: it asserts the callback-executor
    // affinity for the calling member and hands back the implementation.
    Impl* checkedImpl(std::source_location location = std::source_location::current()) const;

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
