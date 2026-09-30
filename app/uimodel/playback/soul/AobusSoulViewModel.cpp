// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
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

    AobusSoulGradientColors aobusSoulGradientColors(AobusSoulRgb const aura, double const hueShiftDegrees) noexcept
    {
      return AobusSoulGradientColors{
        .core = aobusSoulShiftRgb(kAobusSoulUiCyan, hueShiftDegrees),
        .body = aobusSoulShiftRgb(aura, -hueShiftDegrees),
      };
    }

    using ColorVector = std::array<double, 3>;
    using ColorMatrix = std::array<ColorVector, 3>;

    // Ottosson's OKLab transform from linear sRGB, and its inverse.
    constexpr auto kLinearRgbToLms = ColorMatrix{{{0.4122214708, 0.5363325363, 0.0514459929},
                                                  {0.2119034982, 0.6806995451, 0.1073969566},
                                                  {0.0883024619, 0.2817188376, 0.6299787005}}};
    constexpr auto kLmsToOklab = ColorMatrix{{{0.2104542553, 0.7936177850, -0.0040720468},
                                              {1.9779984951, -2.4285922050, 0.4505937099},
                                              {0.0259040371, 0.7827717662, -0.8086757660}}};
    constexpr auto kOklabToLms = ColorMatrix{
      {{1.0, 0.3963377774, 0.2158037573}, {1.0, -0.1055613458, -0.0638541728}, {1.0, -0.0894841775, -1.2914855480}}};
    constexpr auto kLmsToLinearRgb = ColorMatrix{{{4.0767416621, -3.3077115913, 0.2309699292},
                                                  {-1.2684380046, 2.6097574011, -0.3413193965},
                                                  {-0.0041960863, -0.7034186147, 1.7076147010}}};

    constexpr double kSrgbLinearThreshold = 0.04045;
    constexpr double kLinearSrgbThreshold = 0.0031308;
    constexpr double kSrgbLinearSlope = 12.92;
    constexpr double kSrgbGammaOffset = 0.055;
    constexpr double kSrgbGamma = 2.4;

    ColorVector transform(ColorMatrix const& matrix, ColorVector const& vector) noexcept
    {
      auto result = ColorVector{};

      for (std::size_t row = 0; row < matrix.size(); ++row)
      {
        result[row] = (matrix[row][0] * vector[0]) + (matrix[row][1] * vector[1]) + (matrix[row][2] * vector[2]);
      }

      return result;
    }

    ColorVector oklabFromRgb(AobusSoulRgb const color) noexcept
    {
      auto const linear = [](std::uint8_t const channel)
      {
        auto const encoded = static_cast<double>(channel) / kMaxChannelValue;
        return encoded <= kSrgbLinearThreshold
                 ? encoded / kSrgbLinearSlope
                 : std::pow((encoded + kSrgbGammaOffset) / (1.0 + kSrgbGammaOffset), kSrgbGamma);
      };

      auto lms = transform(kLinearRgbToLms, {linear(color.red), linear(color.green), linear(color.blue)});

      for (auto& component : lms)
      {
        component = std::cbrt(component);
      }

      return transform(kLmsToOklab, lms);
    }

    AobusSoulRgb rgbFromOklab(ColorVector const& oklab) noexcept
    {
      auto lms = transform(kOklabToLms, oklab);

      for (auto& component : lms)
      {
        component = component * component * component;
      }

      auto const linearRgb = transform(kLmsToLinearRgb, lms);
      auto const toChannel = [](double const linear)
      {
        auto const clamped = std::clamp(linear, 0.0, 1.0);
        auto const encoded = clamped <= kLinearSrgbThreshold
                               ? clamped * kSrgbLinearSlope
                               : ((1.0 + kSrgbGammaOffset) * std::pow(clamped, 1.0 / kSrgbGamma)) - kSrgbGammaOffset;
        return static_cast<std::uint8_t>(std::clamp(std::lround(encoded * kMaxChannelValue), 0L, kMaxChannelLong));
      };

      return AobusSoulRgb{
        .red = toChannel(linearRgb[0]), .green = toChannel(linearRgb[1]), .blue = toChannel(linearRgb[2])};
    }

    // Perceptual interpolation keeps a cross-fade from dipping through a muddy
    // midpoint, as a per-channel sRGB mix does between distant hues.
    AobusSoulRgb aobusSoulMixOklab(AobusSoulRgb const from, AobusSoulRgb const to, double const fraction) noexcept
    {
      if (fraction <= 0.0)
      {
        return from;
      }

      if (fraction >= 1.0)
      {
        return to;
      }

      auto const fromLab = oklabFromRgb(from);
      auto const toLab = oklabFromRgb(to);
      auto mixed = ColorVector{};

      for (std::size_t component = 0; component < mixed.size(); ++component)
      {
        mixed[component] = fromLab[component] + ((toLab[component] - fromLab[component]) * fraction);
      }

      return rgbFromOklab(mixed);
    }

    constexpr double kSmoothstepSquareWeight = 3.0;
    constexpr double kSmoothstepCubeWeight = 2.0;

    double smoothstep(double const value) noexcept
    {
      return value * value * (kSmoothstepSquareWeight - (kSmoothstepCubeWeight * value));
    }

    // Antiderivative of smoothstep, zero at zero: v^3 - v^4 / 2.
    double smoothstepIntegral(double const value) noexcept
    {
      auto const cube = value * value * value;
      return cube * (1.0 - (value / kSmoothstepCubeWeight));
    }

    struct VitalityStep final
    {
      double vitality = 0.0;
      double motionSeconds = 0.0;
    };

    // Vitality moves linearly toward its target across the transition span
    // while motion speed follows its smoothstep, so resume eases in and pause
    // eases out. Integrating exactly keeps the travelled phase independent of
    // the adapter's frame rate.
    VitalityStep stepVitality(double const vitality, double const target, double const seconds) noexcept
    {
      auto const rate = 1.0 / kAobusSoulTransitionDuration.count();
      auto const distance = target - vitality;
      auto const reachSeconds = std::abs(distance) / rate;
      auto const rampSeconds = std::min(seconds, reachSeconds);
      auto const next = seconds >= reachSeconds ? target : vitality + std::copysign(seconds * rate, distance);
      auto const rampMotion = std::abs(smoothstepIntegral(next) - smoothstepIntegral(vitality)) / rate;

      return VitalityStep{.vitality = next, .motionSeconds = rampMotion + ((seconds - rampSeconds) * smoothstep(next))};
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

  AobusSoulVisualFrame aobusSoulVisualFrame(AobusSoulRgb const aura, AobusSoulMotionFrame const& motion) noexcept
  {
    return AobusSoulVisualFrame{
      .motion = motion,
      .gradientColors = aobusSoulGradientColors(aura, motion.hueShiftDegrees),
    };
  }

  AobusSoulMotionMode aobusSoulMotionMode(audio::Transport const transport) noexcept
  {
    switch (transport)
    {
      case audio::Transport::Playing: return AobusSoulMotionMode::Animating;
      case audio::Transport::Paused: return AobusSoulMotionMode::Frozen;
      case audio::Transport::Idle:
      case audio::Transport::Opening:
      case audio::Transport::Buffering:
      case audio::Transport::Seeking:
      case audio::Transport::Stopping:
      case audio::Transport::Error: return AobusSoulMotionMode::Dormant;
    }

    return AobusSoulMotionMode::Dormant;
  }

  void AobusSoulAnimationState::setMotionMode(AobusSoulMotionMode const motionMode) noexcept
  {
    if (_motionMode == motionMode)
    {
      return;
    }

    auto const previousMode = _motionMode;
    _motionMode = motionMode;

    if (_motionMode == AobusSoulMotionMode::Dormant)
    {
      _elapsed = std::chrono::duration<double>::zero();
      _motionFrame = {};
      _vitality = 0.0;
    }
    else if (_motionMode == AobusSoulMotionMode::Animating && previousMode == AobusSoulMotionMode::Dormant)
    {
      _motionFrame = aobusSoulMotionAt(_elapsed);
    }
  }

  void AobusSoulAnimationState::setAura(AobusSoulRgb const aura) noexcept
  {
    if (!_hasAura)
    {
      _auraFrom = aura;
      _auraTo = aura;
      _auraProgress = 1.0;
      _hasAura = true;
      return;
    }

    if (aura == _auraTo)
    {
      return;
    }

    _auraFrom = currentAura();
    _auraTo = aura;
    _auraProgress = 0.0;
  }

  void AobusSoulAnimationState::advance(std::chrono::duration<double> const delta) noexcept
  {
    if (delta <= std::chrono::duration<double>::zero())
    {
      return;
    }

    _auraProgress = std::min(1.0, _auraProgress + (delta / kAobusSoulTransitionDuration));

    if (_motionMode == AobusSoulMotionMode::Dormant)
    {
      return;
    }

    auto const target = _motionMode == AobusSoulMotionMode::Animating ? 1.0 : 0.0;
    auto const step = stepVitality(_vitality, target, delta.count());
    _vitality = step.vitality;

    if (step.motionSeconds > 0.0)
    {
      _elapsed += std::chrono::duration<double>{step.motionSeconds};
      _motionFrame = aobusSoulMotionAt(_elapsed);
    }
  }

  void AobusSoulAnimationState::settle() noexcept
  {
    _vitality = _motionMode == AobusSoulMotionMode::Animating ? 1.0 : 0.0;
    _auraProgress = 1.0;
  }

  AobusSoulMotionMode AobusSoulAnimationState::motionMode() const noexcept
  {
    return _motionMode;
  }

  bool AobusSoulAnimationState::needsFrames() const noexcept
  {
    return _motionMode == AobusSoulMotionMode::Animating || _vitality > 0.0 || _auraProgress < 1.0;
  }

  std::chrono::duration<double> AobusSoulAnimationState::elapsed() const noexcept
  {
    return _elapsed;
  }

  AobusSoulMotionFrame const& AobusSoulAnimationState::motionFrame() const noexcept
  {
    return _motionFrame;
  }

  AobusSoulVisualFrame AobusSoulAnimationState::visualFrame() const noexcept
  {
    return aobusSoulVisualFrame(currentAura(), _motionFrame);
  }

  AobusSoulRgb AobusSoulAnimationState::currentAura() const noexcept
  {
    return aobusSoulMixOklab(_auraFrom, _auraTo, smoothstep(_auraProgress));
  }

  SoulAura resolveSoulAura(audio::Transport const transport, bool const ready, rt::QualityState const& signal) noexcept
  {
    if (transport != audio::Transport::Playing && transport != audio::Transport::Paused)
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
    auto view = AobusSoulViewState{};
    view.motionMode = aobusSoulMotionMode(state.transport);
    view.aura = resolveSoulAura(state.transport, state.ready, state.quality);

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
