// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace ao::uimodel::playback
{
  struct SeekViewState final
  {
    std::uint32_t durationMs = 0;
    std::uint32_t positionMs = 0;
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

    void seekPreview(std::uint32_t positionMs);
    void seekFinal(std::uint32_t positionMs);

    void refresh(bool immediateUpdate, std::optional<std::uint32_t> optOverridePosition = std::nullopt);

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
} // namespace ao::uimodel::playback
