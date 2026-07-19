// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/rt/PlaybackState.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <numbers>

namespace ao::rt
{
  class PlaybackService;
}

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

  struct AobusSoulRgb final
  {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend bool operator==(AobusSoulRgb const&, AobusSoulRgb const&) = default;
  };

  struct AobusSoulMotionFrame final
  {
    double breath = 0.0;
    double rotationRadians = 0.0;
    double rotationDegrees = 0.0;
    double luminance = 1.0;
    double hueShiftDegrees = 0.0;
  };

  inline constexpr double kAobusSoulGoldenRatio = std::numbers::phi;
  inline constexpr auto kAobusSoulBreathingPeriod = std::chrono::duration<double>{5.119};
  inline constexpr auto kAobusSoulRotationPeriod =
    std::chrono::duration<double>{kAobusSoulBreathingPeriod.count() * kAobusSoulGoldenRatio};
  inline constexpr auto kAobusSoulOpacityPeriod =
    std::chrono::duration<double>{kAobusSoulRotationPeriod.count() * kAobusSoulGoldenRatio};
  inline constexpr auto kAobusSoulHuePeriod =
    std::chrono::duration<double>{kAobusSoulOpacityPeriod.count() * kAobusSoulGoldenRatio};
  inline constexpr double kAobusSoulOpacityFloor = 0.618;
  inline constexpr double kAobusSoulOpacityBase = (1.0 + kAobusSoulOpacityFloor) / 2.0;
  inline constexpr double kAobusSoulOpacityVariance = (1.0 - kAobusSoulOpacityFloor) / 2.0;
  inline constexpr double kAobusSoulCoreGradientStop = 0.382;
  inline constexpr double kAobusSoulMaxHueShiftDegrees = 10.0;

  inline constexpr auto kAobusSoulBrandCyan = AobusSoulRgb{.red = 0x06, .green = 0xB6, .blue = 0xD4};
  inline constexpr auto kAobusSoulUiCyan = AobusSoulRgb{.red = 0x00, .green = 0xE5, .blue = 0xFF};
  inline constexpr auto kAobusSoulAnchorAmber = AobusSoulRgb{.red = 0xF9, .green = 0x73, .blue = 0x16};
  inline constexpr auto kAobusSoulRadiant = AobusSoulRgb{.red = 0xA8, .green = 0x55, .blue = 0xF7};
  inline constexpr auto kAobusSoulFlowing = AobusSoulRgb{.red = 0x10, .green = 0xB9, .blue = 0x81};
  inline constexpr auto kAobusSoulTurbulent = AobusSoulRgb{.red = 0xF5, .green = 0x9E, .blue = 0x0B};
  inline constexpr auto kAobusSoulBurning = AobusSoulRgb{.red = 0xEF, .green = 0x44, .blue = 0x44};
  inline constexpr auto kAobusSoulVeiled = AobusSoulRgb{.red = 0x6B, .green = 0x72, .blue = 0x80};
  inline constexpr auto kAobusSoulNightField = AobusSoulRgb{.red = 0x11, .green = 0x18, .blue = 0x27};

  struct AobusSoulViewState final
  {
    SoulAura aura = SoulAura::Dormant;
    bool isBreathing = false;
  };

  SoulAura resolveSoulAura(bool playing, bool ready, rt::QualityState const& signal) noexcept;
  AobusSoulRgb aobusSoulAuraRgb(SoulAura aura) noexcept;
  AobusSoulRgb aobusSoulShiftRgb(AobusSoulRgb color, double shiftDegrees) noexcept;
  AobusSoulRgb aobusSoulMixRgb(AobusSoulRgb from, AobusSoulRgb to, double fraction) noexcept;
  AobusSoulRgb aobusSoulScaleRgb(AobusSoulRgb color, double factor) noexcept;
  AobusSoulMotionFrame aobusSoulMotionAt(std::chrono::duration<double> elapsed) noexcept;

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

    async::Subscription _snapshotSub;
  };
} // namespace ao::uimodel
