// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
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

  enum class PlaybackRelativeSeekEndBehavior : std::uint8_t
  {
    Clamp,
    Next,
  };

  /**
   * The mutation side of the playback boundary. Commands are
   * callback-executor-affine. Small transport commands complete within one
   * executor turn and report no completion token; a view start returns its
   * immediate execution result or successful queue admission. Session save,
   * restore, and discard keep call-level results on `AppRuntime` because their
   * callers act on them.
   * Once shutdown closes admission, void commands are ignored and a view start
   * returns `InvalidState`.
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
    /**
     * Admits Next only while the expected playback occurrence is still the
     * runtime subject and, for active audio, Engine's current item. A matching
     * deferred Idle restore is also admissible. Rejection never queues.
     */
    virtual bool tryNext(PlaybackOccurrenceId expectedOccurrenceId) = 0;
    virtual void previous() = 0;
    virtual void clearSequence() = 0;
    virtual void setPlaybackMode(ShuffleMode shuffle, RepeatMode repeat) = 0;
    virtual void setShuffleMode(ShuffleMode mode) = 0;
    virtual void setRepeatMode(RepeatMode mode) = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void seek(std::chrono::milliseconds elapsed, PlaybackSeekMode mode = PlaybackSeekMode::Final) = 0;
    /**
     * Queues or synchronously executes a seek whose occurrence is validated at
     * execution. A stale request issues no seek and publishes no seek update.
     */
    virtual void seek(PlaybackOccurrenceId expectedOccurrenceId,
                      std::chrono::milliseconds elapsed,
                      PlaybackSeekMode mode = PlaybackSeekMode::Final) = 0;

    /**
     * Queues a guarded final seek relative to the execution-time live position.
     * The captured occurrence must still be current when the command executes.
     * Next advances only for a positive offset strictly past the live endpoint;
     * without an admissible successor it does nothing. Clamp is the UI default.
     */
    virtual void seekBy(PlaybackOccurrenceId expectedOccurrenceId,
                        std::chrono::milliseconds delta,
                        PlaybackRelativeSeekEndBehavior endBehavior = PlaybackRelativeSeekEndBehavior::Clamp) = 0;

    /**
     * Issues a final seek only when the expected playback occurrence is still
     * current. This synchronous-only command rejects rather than queues while
     * another command or publication is active.
     */
    virtual bool trySeek(PlaybackOccurrenceId expectedOccurrenceId, std::chrono::milliseconds elapsed) = 0;

    virtual void setOutputDevice(audio::BackendId const& backendId,
                                 audio::DeviceId const& deviceId,
                                 audio::ProfileId const& profileId) = 0;
    virtual void setVolume(float volume) = 0;
    virtual void setMuted(bool muted) = 0;

    // Reveal emits an application-navigation request without mutating transport
    // or succession state.
    virtual void revealPlayingTrack() = 0;
    virtual void revealTrack(TrackId trackId,
                             ViewId preferredViewId = kInvalidViewId,
                             ListId preferredListId = kInvalidListId) = 0;

  protected:
    PlaybackCommands() = default;
  };
} // namespace ao::rt
