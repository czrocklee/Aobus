// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/uimodel/playback/seek/PlaybackClockChangeFilter.h>

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
  struct SeekViewState final
  {
    std::chrono::milliseconds duration{0};
    std::chrono::milliseconds elapsed{0};
    bool isPlaying = false;
    bool enabled = false;
    bool immediateUpdate = false;
  };

  class SeekViewModel final
  {
  public:
    SeekViewModel(rt::PlaybackService& playback, std::function<void(SeekViewState const&)> onRender);

    SeekViewModel(SeekViewModel const&) = delete;
    SeekViewModel& operator=(SeekViewModel const&) = delete;
    SeekViewModel(SeekViewModel&&) = delete;
    SeekViewModel& operator=(SeekViewModel&&) = delete;

    ~SeekViewModel() = default;

    void seekPreview(std::chrono::milliseconds elapsed);
    void seekFinal(std::chrono::milliseconds elapsed);
    void seekBy(std::chrono::milliseconds delta);

    void refresh(bool immediateUpdate, std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

  private:
    // Refreshes only when the transport-relevant subset of the snapshot changes,
    // so unrelated publications (volume, quality) do not reset the seek clock.
    void onSnapshotChanged(rt::PlaybackSnapshot const& snapshot);
    void render(rt::PlaybackTransportSnapshot const& state,
                bool immediateUpdate,
                std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _commands;
    std::function<void(SeekViewState const&)> _onRender;

    detail::PlaybackClockChangeFilter _clockChangeFilter;

    async::Subscription _snapshotSub;
    async::Subscription _seekPreviewSub;
  };
} // namespace ao::uimodel
