// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>

#include <chrono>
#include <cstdint>

namespace ao::rt
{
  enum class PlaybackSeekMode : std::uint8_t
  {
    Final,
    Preview,
  };

  /**
   * The mutation side of the playback boundary. Commands are
   * callback-executor-affine. Small transport commands complete within one
   * executor turn and report no completion token; a view start returns a
   * synchronous admission result, and its asynchronous outcome is observed on
   * the failure event and the next snapshot. Session save, restore, and discard
   * keep call-level results on `AppRuntime` because their callers act on them.
   */
  class PlaybackCommands
  {
  public:
    virtual ~PlaybackCommands() = default;

    PlaybackCommands(PlaybackCommands const&) = delete;
    PlaybackCommands& operator=(PlaybackCommands const&) = delete;
    PlaybackCommands(PlaybackCommands&&) = delete;
    PlaybackCommands& operator=(PlaybackCommands&&) = delete;

    /** Start playback of a track from a view's live source context. */
    virtual Result<> startFromView(ViewId viewId, TrackId startTrackId) = 0;

    virtual void next() = 0;
    virtual void previous() = 0;
    virtual void clearSequence() = 0;
    virtual void setShuffleMode(ShuffleMode mode) = 0;
    virtual void setRepeatMode(RepeatMode mode) = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void seek(std::chrono::milliseconds elapsed, PlaybackSeekMode mode = PlaybackSeekMode::Final) = 0;
    virtual void setOutputDevice(audio::BackendId const& backendId,
                                 audio::DeviceId const& deviceId,
                                 audio::ProfileId const& profileId) = 0;
    virtual void setVolume(float volume) = 0;
    virtual void setMuted(bool muted) = 0;

    // Reveal remains on the boundary temporarily; RFC 0005 stage 6 moves it to
    // an explicit navigation intent above playback.
    virtual void revealPlayingTrack() = 0;
    virtual void revealTrack(TrackId trackId,
                             ViewId preferredViewId = kInvalidViewId,
                             ListId preferredListId = kInvalidListId) = 0;

  protected:
    PlaybackCommands() = default;
  };
} // namespace ao::rt
