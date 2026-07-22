// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PlaybackSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ViewIds.h>

#include <chrono>
#include <functional>

namespace ao::rt
{
  using PlaybackSnapshotObserver = std::move_only_function<void(PlaybackSnapshot const&)>;

  /** Temporary navigation request retained until RFC 0005 stage 6. */
  struct PlaybackRevealTrackRequest final
  {
    TrackId trackId = kInvalidTrackId;
    ViewId preferredViewId = kInvalidViewId;
    ListId preferredListId = kInvalidListId;
  };

  /**
   * Playback publication and transient-event subscriptions. The current
   * snapshot remains owned and read directly through `PlaybackService`; this role
   * reports when a new snapshot is published and carries events that are not
   * self-contained application state. Every method is
   * callback-executor-affine; handlers run on the executor thread and must
   * defer owner teardown to a later turn. A playback command issued by a
   * handler is admitted into the service command queue and executes in a later
   * executor turn; it cannot alter the event currently being delivered.
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
    virtual async::Subscription onSeekPreview(std::move_only_function<void(std::chrono::milliseconds)> handler) = 0;
    virtual async::Subscription onRevealTrackRequested(
      std::move_only_function<void(PlaybackRevealTrackRequest const&)> handler) = 0;

  protected:
    PlaybackEvents() = default;
  };
} // namespace ao::rt
