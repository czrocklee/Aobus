// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>

#include <chrono>
#include <cstdint>

namespace ao::rt
{
  /** Identity of the current playback-clock anchor. */
  struct PlaybackPositionRevision final
  {
    std::uint64_t value = 0;

    auto operator<=>(PlaybackPositionRevision const&) const = default;
    bool operator==(PlaybackPositionRevision const&) const = default;
  };

  /** Identity of the most recently committed final seek. */
  struct PlaybackFinalSeekRevision final
  {
    std::uint64_t value = 0;

    auto operator<=>(PlaybackFinalSeekRevision const&) const = default;
    bool operator==(PlaybackFinalSeekRevision const&) const = default;
  };

  /**
   * Public source-lifecycle classification for the current succession subject.
   * Mirrors the internal sequence source state so consumers do not depend on
   * the runtime-internal succession owner type.
   */
  enum class PlaybackSourceState : std::uint8_t
  {
    Inactive,
    Live,
    Invalidated,
  };

  /** Application transport portion of a coherent playback snapshot. */
  struct PlaybackTransportSnapshot final
  {
    audio::Transport transport = audio::Transport::Idle;
    bool ready = false;
    PlaybackPositionRevision positionRevision{};
    PlaybackFinalSeekRevision finalSeekRevision{};
    std::chrono::milliseconds elapsed{0};
    std::chrono::milliseconds duration{0};
    NowPlayingInfo nowPlaying{};
    VolumeState volume{};
    OutputState output{};
    QualityState quality{};

    /**
     * Semantic equality excludes the correlated elapsed clock sample. Position
     * discontinuities remain content through their explicit revisions.
     */
    bool operator==(PlaybackTransportSnapshot const& other) const
    {
      return transport == other.transport && ready == other.ready && positionRevision == other.positionRevision &&
             finalSeekRevision == other.finalSeekRevision && duration == other.duration &&
             nowPlaying == other.nowPlaying && volume == other.volume && output == other.output &&
             quality == other.quality;
    }
  };

  /** Live-source succession portion of a coherent playback snapshot. */
  struct PlaybackSuccessionSnapshot final
  {
    PlaybackSourceState sourceState = PlaybackSourceState::Inactive;
    TrackId currentTrackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    bool hasNext = false;
    bool hasPrevious = false;
    ShuffleMode shuffle = ShuffleMode::Off;
    RepeatMode repeat = RepeatMode::Off;

    bool operator==(PlaybackSuccessionSnapshot const&) const = default;
  };

  /**
   * One immutable application playback state. Every logical transition that
   * changes semantic content publishes exactly one snapshot; a no-op publishes
   * none. Observers read a coherent transport and succession view rather than
   * assembling it from independent services. The transport quality and output
   * travel with the snapshot so they cannot advance as unrelated public state.
   */
  struct PlaybackSnapshot final
  {
    PlaybackTransportSnapshot transport{};
    PlaybackSuccessionSnapshot succession{};

    bool operator==(PlaybackSnapshot const&) const = default;
  };
} // namespace ao::rt
