// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ao::uimodel::playback
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

    rt::PlaybackService& _playback;
    std::function<void(PlaybackTimeViewState const&)> _onRender;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
    rt::Subscription _seekUpdateSub;
  };
} // namespace ao::uimodel::playback
