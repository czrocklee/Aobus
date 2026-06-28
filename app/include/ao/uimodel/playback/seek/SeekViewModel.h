// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <chrono>
#include <functional>
#include <optional>

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

    void refresh(bool immediateUpdate, std::optional<std::chrono::milliseconds> optOverrideElapsed = std::nullopt);

  private:
    rt::PlaybackService& _playback;
    std::function<void(SeekViewState const&)> _onRender;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
    rt::Subscription _seekUpdateSub;
  };
} // namespace ao::uimodel
