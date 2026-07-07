// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <cstdint>
#include <functional>

namespace ao::uimodel
{
  enum class SoulAura : std::uint8_t
  {
    Dormant,
    Veiled,
    Radiant,
    Flowing,
    Turbulent,
    Burning,
  };

  struct AobusSoulViewState final
  {
    SoulAura aura = SoulAura::Dormant;
    bool isBreathing = false;
  };

  SoulAura resolveSoulAura(bool playing, bool ready, rt::QualityState const& signal) noexcept;

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
    rt::Subscription _outputDeviceSub;
    rt::Subscription _startedSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _idleSub;
  };
} // namespace ao::uimodel
