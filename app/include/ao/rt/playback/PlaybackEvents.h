// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PlaybackSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/ViewIds.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace ao::rt
{
  using PlaybackSnapshotObserver = std::move_only_function<void(PlaybackSnapshot const&)>;

  /** Temporary navigation intent retained until RFC 0005 stage 6. */
  struct PlaybackRevealTrackRequest final
  {
    TrackId trackId = kInvalidTrackId;
    ViewId preferredViewId = kInvalidViewId;
    ListId preferredListId = kInvalidListId;
  };

  /**
   * Correlation identity for one accepted public playback command. It lets an
   * asynchronous failure event name the command it belongs to once preparation
   * moves off the callback executor. Until then it is absent.
   */
  struct PlaybackCommandId final
  {
    std::uint64_t value = 0;

    bool operator==(PlaybackCommandId const&) const = default;
  };

  /**
   * A playback failure carried with the application revision current when it was
   * observed and, once available, the command it resulted from. A consumer that
   * has already observed a newer snapshot may discard an older failure rather
   * than combining it with current state.
   */
  struct PlaybackFailureEvent final
  {
    PlaybackRevision revision{};
    std::optional<PlaybackCommandId> optCommandId{};
    PlaybackFailure failure{};
  };

  /**
   * A non-committing seek preview tagged with the revision it refines. Preview
   * scrubbing does not advance the application revision; a final seek commits
   * into the snapshot's transport position instead.
   */
  struct PlaybackSeekPreview final
  {
    PlaybackRevision revision{};
    std::chrono::milliseconds elapsed{0};
  };

  /**
   * Playback publication and transient-event subscriptions. The current
   * snapshot remains owned and read directly through `PlaybackService`; this role
   * reports when a new snapshot is published and carries revisioned events that
   * are not self-contained application state. Every method is
   * callback-executor-affine; handlers run on the executor thread and must
   * defer owner teardown to a later turn.
   */
  class PlaybackEvents
  {
  public:
    virtual ~PlaybackEvents() = default;

    PlaybackEvents(PlaybackEvents const&) = delete;
    PlaybackEvents& operator=(PlaybackEvents const&) = delete;
    PlaybackEvents(PlaybackEvents&&) = delete;
    PlaybackEvents& operator=(PlaybackEvents&&) = delete;

    virtual async::Subscription onSnapshot(PlaybackSnapshotObserver observer) = 0;
    virtual async::Subscription onPlaybackFailure(
      std::move_only_function<void(PlaybackFailureEvent const&)> handler) = 0;
    virtual async::Subscription onSeekPreview(std::move_only_function<void(PlaybackSeekPreview const&)> handler) = 0;
    virtual async::Subscription onRevealTrackRequested(
      std::move_only_function<void(PlaybackRevealTrackRequest const&)> handler) = 0;

  protected:
    PlaybackEvents() = default;
  };
} // namespace ao::rt
