// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <cstdint>
#include <functional>

namespace ao::uimodel
{
  enum class AuraColor : std::uint8_t
  {
    Idle,
    Unknown,
    Perfect,
    Lossless,
    Intervention,
    Clipped,
  };

  struct AobusSoulViewState final
  {
    AuraColor auraColor = AuraColor::Idle;
    bool isBreathing = false;
  };

  class AobusSoulViewModel final
  {
  public:
    AobusSoulViewModel(rt::PlaybackService& playback, std::function<void(AobusSoulViewState const&)> onRender);

    AobusSoulViewModel(AobusSoulViewModel const&) = delete;
    AobusSoulViewModel& operator=(AobusSoulViewModel const&) = delete;
    AobusSoulViewModel(AobusSoulViewModel&&) = delete;
    AobusSoulViewModel& operator=(AobusSoulViewModel&&) = delete;

    ~AobusSoulViewModel() = default;

  private:
    void refresh();

    rt::PlaybackService& _playback;
    std::function<void(AobusSoulViewState const&)> _onRender;

    rt::Subscription _qualitySub;
    rt::Subscription _outputSub;
    rt::Subscription _startedSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _idleSub;
  };
} // namespace ao::uimodel
