// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace ao::rt
{
  class PlaybackService;
  struct PlaybackSnapshot;
  struct PlaybackTransportSnapshot;
}

namespace ao::uimodel
{
  struct SeekSliderUpdate;

  /** Render state shared by playback-position controls. */
  struct PlaybackPositionViewState final
  {
    rt::PlaybackOccurrenceId occurrenceId{};
    std::chrono::milliseconds duration{0};
    std::chrono::milliseconds elapsed{0};
    bool hasTrack = false;
    // Controls clock interpolation, not whether the transport can be paused.
    bool isPlaying = false;
    // A retained track can still display its clock after seek ownership ends.
    bool seekable = false;
    // True for a transient seek preview: `elapsed` then carries the preview
    // position rather than the transport's own clock.
    bool isPreviewing = false;
    bool immediateUpdate = false;
  };

  PlaybackPositionViewState playbackPositionView(rt::PlaybackTransportSnapshot const& state) noexcept;

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

    void seekPreview(rt::PlaybackOccurrenceId expectedOccurrenceId, std::chrono::milliseconds elapsed);
    void seekFinal(rt::PlaybackOccurrenceId expectedOccurrenceId, std::chrono::milliseconds elapsed);
    void cancelPointerSeek();
    // Discrete steps hold their displayed target until the guarded Final settles.
    // A pointer release finishes its existing shared preview instead.
    void commitSliderUpdate(SeekSliderUpdate const& update);
    bool hasPendingSliderFinal() const noexcept { return _pendingSliderOccurrencePtr != nullptr; }
    void seekBy(std::chrono::milliseconds delta);

  private:
    void submitFinal(rt::PlaybackOccurrenceId occurrenceId,
                     std::chrono::milliseconds elapsed,
                     compat::MoveOnlyFunction<void()> onSettled);
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
    TrackId _clockTrackId = kInvalidTrackId;
    rt::PlaybackOccurrenceId _clockOccurrenceId{};
    rt::PlaybackPositionRevision _clockPositionRevision{};
    std::chrono::milliseconds _clockDuration{0};

    std::shared_ptr<rt::PlaybackOccurrenceId const> _pendingSliderOccurrencePtr;
    async::Subscription _snapshotSub;
    rt::PlaybackPointerSeek _pointerSeek{};
    async::Subscription _seekPreviewSub;
    async::Subscription _seekPreviewEndedSub;
  };
} // namespace ao::uimodel
