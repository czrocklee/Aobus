// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    constexpr double kFullCircleDegrees = 360.0;
    constexpr double kHalfScale = 2.0;
    constexpr double kHueSectorGreenOffset = 2.0;
    constexpr double kHueSectorBlueOffset = 4.0;
    constexpr double kHueSectorWrap = 6.0;
    constexpr std::int32_t kHueSectorCount = 6;
    constexpr double kMaxChannelValue = 255.0;
    constexpr long kMaxChannelLong = 255L;

    double soulPhase(std::chrono::duration<double> const elapsed, std::chrono::duration<double> const period) noexcept
    {
      auto const clamped = std::max(0.0, elapsed.count());
      auto const cycleElapsed = std::fmod(clamped, period.count());
      return kHalfScale * std::numbers::pi * cycleElapsed / period.count();
    }
  } // namespace

  AobusSoulRgb aobusSoulAuraRgb(SoulAura const aura) noexcept
  {
    switch (aura)
    {
      case SoulAura::Dormant: return kAobusSoulUiCyan;
      case SoulAura::Veiled: return kAobusSoulVeiled;
      case SoulAura::Radiant: return kAobusSoulRadiant;
      case SoulAura::Flowing: return kAobusSoulFlowing;
      case SoulAura::Turbulent: return kAobusSoulTurbulent;
      case SoulAura::Burning: return kAobusSoulBurning;
    }

    return kAobusSoulVeiled;
  }

  AobusSoulRgb aobusSoulMixRgb(AobusSoulRgb const from, AobusSoulRgb const to, double const fraction) noexcept
  {
    auto const clamped = std::clamp(fraction, 0.0, 1.0);
    auto const mixChannel = [clamped](std::uint8_t const fromChannel, std::uint8_t const toChannel)
    { return static_cast<std::uint8_t>(std::lround(fromChannel + ((toChannel - fromChannel) * clamped))); };

    return AobusSoulRgb{.red = mixChannel(from.red, to.red),
                        .green = mixChannel(from.green, to.green),
                        .blue = mixChannel(from.blue, to.blue)};
  }

  AobusSoulRgb aobusSoulScaleRgb(AobusSoulRgb const color, double const factor) noexcept
  {
    auto const scaleChannel = [factor](std::uint8_t const value)
    { return static_cast<std::uint8_t>(std::clamp(std::lround(value * factor), 0L, kMaxChannelLong)); };

    return AobusSoulRgb{
      .red = scaleChannel(color.red), .green = scaleChannel(color.green), .blue = scaleChannel(color.blue)};
  }

  AobusSoulRgb aobusSoulShiftRgb(AobusSoulRgb const color, double const shiftDegrees) noexcept
  {
    constexpr double kMinShiftDegrees = 0.01;

    if (std::abs(shiftDegrees) < kMinShiftDegrees)
    {
      return color;
    }

    double const red = static_cast<double>(color.red) / kMaxChannelValue;
    double const green = static_cast<double>(color.green) / kMaxChannelValue;
    double const blue = static_cast<double>(color.blue) / kMaxChannelValue;
    double const maxValue = std::max({red, green, blue});
    double const minValue = std::min({red, green, blue});
    double const delta = maxValue - minValue;
    double const saturation = maxValue == 0.0 ? 0.0 : delta / maxValue;
    double hue = 0.0;

    if (delta > 0.0)
    {
      if (maxValue == red)
      {
        hue = ((green - blue) / delta) + (green < blue ? kHueSectorWrap : 0.0);
      }
      else if (maxValue == green)
      {
        hue = ((blue - red) / delta) + kHueSectorGreenOffset;
      }
      else
      {
        hue = ((red - green) / delta) + kHueSectorBlueOffset;
      }

      hue /= kHueSectorWrap;
    }

    hue = std::fmod(hue + (shiftDegrees / kFullCircleDegrees), 1.0);

    if (hue < 0.0)
    {
      hue += 1.0;
    }

    double const scaledHue = hue * static_cast<double>(kHueSectorCount);
    std::int32_t const sector = static_cast<std::int32_t>(scaledHue);
    double const fraction = scaledHue - static_cast<double>(sector);
    double const lowerValue = maxValue * (1.0 - saturation);
    double const descendingValue = maxValue * (1.0 - (fraction * saturation));
    double const ascendingValue = maxValue * (1.0 - ((1.0 - fraction) * saturation));

    auto const toChannel = [](double const value)
    { return static_cast<std::uint8_t>(std::clamp(std::lround(value * kMaxChannelValue), 0L, kMaxChannelLong)); };

    switch (sector % kHueSectorCount)
    {
      case 0:
        return AobusSoulRgb{
          .red = toChannel(maxValue), .green = toChannel(ascendingValue), .blue = toChannel(lowerValue)};
      case 1:
        return AobusSoulRgb{
          .red = toChannel(descendingValue), .green = toChannel(maxValue), .blue = toChannel(lowerValue)};
      case 2:
        return AobusSoulRgb{
          .red = toChannel(lowerValue), .green = toChannel(maxValue), .blue = toChannel(ascendingValue)};
      case 3:
        return AobusSoulRgb{
          .red = toChannel(lowerValue), .green = toChannel(descendingValue), .blue = toChannel(maxValue)};
      case 4:
        return AobusSoulRgb{
          .red = toChannel(ascendingValue), .green = toChannel(lowerValue), .blue = toChannel(maxValue)};
      default:
        return AobusSoulRgb{
          .red = toChannel(maxValue), .green = toChannel(lowerValue), .blue = toChannel(descendingValue)};
    }
  }

  AobusSoulMotionFrame aobusSoulMotionAt(std::chrono::duration<double> const elapsed) noexcept
  {
    auto const breathingPhase = soulPhase(elapsed, kAobusSoulBreathingPeriod);
    auto const rotationRadians = soulPhase(elapsed, kAobusSoulRotationPeriod);
    auto const opacityPhase = soulPhase(elapsed, kAobusSoulOpacityPeriod);
    auto const huePhase = soulPhase(elapsed, kAobusSoulHuePeriod);

    return AobusSoulMotionFrame{
      .breath = (std::sin(breathingPhase) + 1.0) / kHalfScale,
      .rotationRadians = rotationRadians,
      .rotationDegrees = rotationRadians * kFullCircleDegrees / (kHalfScale * std::numbers::pi),
      .luminance = kAobusSoulOpacityBase + (kAobusSoulOpacityVariance * std::sin(opacityPhase)),
      .hueShiftDegrees = kAobusSoulMaxHueShiftDegrees * std::sin(huePhase)};
  }

  SoulAura resolveSoulAura(bool const playing, bool const ready, rt::QualityState const& signal) noexcept
  {
    if (!playing)
    {
      return SoulAura::Dormant;
    }

    if (!ready)
    {
      return SoulAura::Veiled;
    }

    if (signal.overall == audio::Quality::Clipped || signal.pipelineQuality == audio::Quality::Clipped)
    {
      return SoulAura::Burning;
    }

    if (signal.pipelineQuality == audio::Quality::LinearIntervention)
    {
      return SoulAura::Turbulent;
    }

    if (!signal.fullyVerified || signal.sourceQuality == audio::Quality::LossySource ||
        signal.sourceQuality == audio::Quality::Unknown)
    {
      return SoulAura::Veiled;
    }

    switch (signal.pipelineQuality)
    {
      case audio::Quality::BitwisePerfect: return SoulAura::Radiant;
      case audio::Quality::LosslessPadded:
      case audio::Quality::LosslessFloat: return SoulAura::Flowing;
      case audio::Quality::LinearIntervention: return SoulAura::Turbulent;
      case audio::Quality::Clipped: return SoulAura::Burning;
      case audio::Quality::LossySource:
      case audio::Quality::Unknown: return SoulAura::Veiled;
    }

    return SoulAura::Veiled;
  }

  AobusSoulViewModel::AobusSoulViewModel(rt::PlaybackService& playback,
                                         std::function<void(AobusSoulViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
    refresh();
  }

  void AobusSoulViewModel::refresh()
  {
    render(_playback.snapshot().transport);
  }

  void AobusSoulViewModel::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    render(snapshot.transport);
  }

  void AobusSoulViewModel::render(rt::PlaybackTransportSnapshot const& state)
  {
    bool const playing = (state.transport == audio::Transport::Playing);

    auto view = AobusSoulViewState{};
    view.isBreathing = playing;
    view.aura = resolveSoulAura(playing, state.ready, state.quality);

    if (_hasLastView && view == _lastView)
    {
      return;
    }

    _lastView = view;
    _hasLastView = true;

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
