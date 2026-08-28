// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>
#include <functional>
#include <optional>

namespace ao::rt
{
  class PlaybackService;
  struct PlaybackSnapshot;
  struct PlaybackTransportSnapshot;
}

namespace ao::uimodel
{
  /** Render state shared by playback-position controls. */
  struct PlaybackPositionViewState final
  {
    std::chrono::milliseconds duration{0};
    std::chrono::milliseconds elapsed{0};
    bool isPlaying = false;
    // Whether a position can be chosen at all: false for a track with no known
    // duration, such as a stream.
    bool seekable = false;
    // True only while the user is dragging: `elapsed` then carries the preview
    // position rather than the transport's own clock.
    bool isPreviewing = false;
    bool immediateUpdate = false;
  };

  /**
   * Publishes the current playback position and accepts new ones.
   *
   * Both the scrubber and the time readout consume the same state; each reads
   * the subset it renders.
   */
  class PlaybackPositionViewModel final
  {
  public:
    PlaybackPositionViewModel(rt::PlaybackService& playback,
                              std::function<void(PlaybackPositionViewState const&)> onRender);

    PlaybackPositionViewModel(PlaybackPositionViewModel const&) = delete;
    PlaybackPositionViewModel& operator=(PlaybackPositionViewModel const&) = delete;
    PlaybackPositionViewModel(PlaybackPositionViewModel&&) = delete;
    PlaybackPositionViewModel& operator=(PlaybackPositionViewModel&&) = delete;

    ~PlaybackPositionViewModel() = default;

    void seekPreview(std::chrono::milliseconds elapsed);
    void seekFinal(std::chrono::milliseconds elapsed);
    void seekBy(std::chrono::milliseconds delta);

  private:
    void refresh(bool immediateUpdate,
                 bool isPreviewing = false,
                 std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

    // Refreshes only when the transport-relevant subset of the snapshot changes,
    // so unrelated publications (volume, quality) do not reset the playback clock.
    void onSnapshotChanged(rt::PlaybackSnapshot const& snapshot);
    void render(rt::PlaybackTransportSnapshot const& state,
                bool immediateUpdate,
                bool isPreviewing,
                std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _commands;
    std::function<void(PlaybackPositionViewState const&)> _onRender;

    audio::Transport _clockTransport = audio::Transport::Idle;
    rt::PlaybackPositionRevision _clockPositionRevision{};
    std::chrono::milliseconds _clockDuration{0};

    async::Subscription _snapshotSub;
    async::Subscription _seekPreviewSub;
  };
} // namespace ao::uimodel
