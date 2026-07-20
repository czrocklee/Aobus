// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/uimodel/playback/seek/PlaybackClockChangeFilter.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ao::rt
{
  class PlaybackService;
  struct PlaybackSnapshot;
  struct PlaybackTransportSnapshot;
}

namespace ao::uimodel
{
  enum class PlaybackTimeMode : std::uint8_t
  {
    Default,
    Elapsed,
    Duration
  };

  struct PlaybackTimeViewState final
  {
    std::chrono::milliseconds duration{0};
    std::chrono::milliseconds elapsed{0};
    bool isPlaying = false;
    bool isPreviewing = false;
    bool immediateUpdate = false;
  };

  class PlaybackTimeViewModel final
  {
  public:
    PlaybackTimeViewModel(rt::PlaybackService& playback, std::function<void(PlaybackTimeViewState const&)> onRender);

    PlaybackTimeViewModel(PlaybackTimeViewModel const&) = delete;
    PlaybackTimeViewModel& operator=(PlaybackTimeViewModel const&) = delete;
    PlaybackTimeViewModel(PlaybackTimeViewModel&&) = delete;
    PlaybackTimeViewModel& operator=(PlaybackTimeViewModel&&) = delete;

    ~PlaybackTimeViewModel() = default;

    static std::string describeTimeTemplate(PlaybackTimeMode mode);
    static std::string formatPlaybackTime(PlaybackTimeMode mode,
                                          std::chrono::milliseconds elapsed,
                                          std::chrono::milliseconds duration);

  private:
    void refresh(bool immediateUpdate,
                 bool isPreviewing,
                 std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);
    void render(rt::PlaybackTransportSnapshot const& state,
                bool immediateUpdate,
                bool isPreviewing,
                std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

    // Refreshes only when the transport-relevant subset of the snapshot changes,
    // so unrelated publications (volume, quality) do not reset the playback clock.
    void onSnapshotChanged(rt::PlaybackSnapshot const& snapshot);

    rt::PlaybackService& _playback;
    std::function<void(PlaybackTimeViewState const&)> _onRender;

    detail::PlaybackClockChangeFilter _clockChangeFilter;

    async::Subscription _snapshotSub;
    async::Subscription _seekPreviewSub;
  };
} // namespace ao::uimodel
