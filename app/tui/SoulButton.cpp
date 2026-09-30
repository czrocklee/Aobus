// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SoulButton.h"

#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    // The soul button keeps the GTK AobusSoul color and timing recipe, but draws
    // a partial arc on a three-cell braille canvas. The arc is positioned by
    // distance along the ring rather than by angle, so it advances one dot at a
    // time at an even pace. GTK's stroke-width breathing becomes arc length with
    // the same golden expansion, and the GTK cyan-core gradient travels with the
    // arc: the head carries the core and the tail carries the quality aura.
    constexpr std::int32_t kSoulGlyphColumns = 3;
    constexpr double kSoulTransientPulseDivisor = 4.0;
    constexpr double kTransientSoulLuminance = 0.9;
    constexpr double kDormantSoulLuminance = 0.5;
    constexpr double kFullTurnRadians = 2.0 * std::numbers::pi;
    constexpr auto kSoulTransientPulsePeriod =
      std::chrono::duration<double>{uimodel::kAobusSoulBreathingPeriod.count() / kSoulTransientPulseDivisor};

    struct SoulDot final
    {
      std::int32_t column = 0;
      std::int32_t row = 0;
    };

    // The 6x4 braille ring, clockwise from its top-left dot.
    constexpr auto kSoulRingDots = std::to_array<SoulDot>({
      {.column = 1, .row = 0},
      {.column = 2, .row = 0},
      {.column = 3, .row = 0},
      {.column = 4, .row = 0},
      {.column = 5, .row = 1},
      {.column = 5, .row = 2},
      {.column = 4, .row = 3},
      {.column = 3, .row = 3},
      {.column = 2, .row = 3},
      {.column = 1, .row = 3},
      {.column = 0, .row = 2},
      {.column = 0, .row = 1},
    });

    // Inner dots thicken the arc's left and right sides. Each follows the ring
    // dot beside it, but only once the arc fully covers that dot, so arc tips
    // stay a single dot wide.
    struct SoulInnerDot final
    {
      SoulDot dot{};
      std::size_t ringIndex = 0;
    };

    constexpr auto kSoulInnerDots = std::to_array<SoulInnerDot>({
      {.dot = {.column = 4, .row = 1}, .ringIndex = 4},
      {.dot = {.column = 4, .row = 2}, .ringIndex = 5},
      {.dot = {.column = 1, .row = 2}, .ringIndex = 10},
      {.dot = {.column = 1, .row = 1}, .ringIndex = 11},
    });

    // Neighbouring ring dots are one dot apart, or one diagonal at the corners.
    consteval std::array<double, kSoulRingDots.size() + 1> soulRingDistances()
    {
      auto distances = std::array<double, kSoulRingDots.size() + 1>{};

      for (std::size_t index = 0; index < kSoulRingDots.size(); ++index)
      {
        auto const& from = kSoulRingDots[index];
        auto const& to = kSoulRingDots[(index + 1) % kSoulRingDots.size()];
        auto const isDiagonal = from.column != to.column && from.row != to.row;
        distances[index + 1] = distances[index] + (isDiagonal ? std::numbers::sqrt2 : 1.0);
      }

      return distances;
    }

    constexpr auto kSoulRingDistances = soulRingDistances();
    constexpr double kSoulRingPerimeter = kSoulRingDistances.back();
    // Rotation zero centers the arc on the ring's right side.
    constexpr double kSoulArcOrigin = (kSoulRingDistances[4] + kSoulRingDistances[5]) / 2.0;
    // Arc length in dots at exhale; inhale expands it by the golden ratio, as
    // GTK expands its stroke width.
    constexpr double kSoulArcExhaleLength = 4.6;
    constexpr double kSoulArcInhaleLength = kSoulArcExhaleLength * uimodel::kAobusSoulGoldenRatio;
    // A dot is drawn once the arc covers at least half of it.
    constexpr double kSoulDotDrawnCoverage = 0.5;
    // A cell whose best dot is only half covered dims to this share of its
    // luminance, so dots fade in at the head and out at the tail instead of
    // popping.
    constexpr double kSoulEdgeLuminanceFloor = 0.45;

    struct SoulCell final
    {
      std::uint8_t dots = 0;
      // Arc position of the cell's leading dot: zero at the head, one at the tail.
      double headDistance = 1.0;
      // Coverage of the cell's most covered dot.
      double coverage = 1.0;
    };

    using SoulCanvas = std::array<SoulCell, static_cast<std::size_t>(kSoulGlyphColumns)>;

    struct SoulDotSample final
    {
      double coverage = 0.0;
      double headDistance = 0.0;
    };

    std::uint8_t brailleDotBit(SoulDot const dot)
    {
      // Unicode braille numbers the dots down the left column first, then the
      // right, with the bottom row appended as dots 7 and 8.
      constexpr auto kBrailleDotBits = std::to_array<std::array<std::uint8_t, 2>>({
        {0x01, 0x08},
        {0x02, 0x10},
        {0x04, 0x20},
        {0x40, 0x80},
      });

      return kBrailleDotBits[static_cast<std::size_t>(dot.row)][static_cast<std::size_t>(dot.column % 2)];
    }

    std::string brailleGlyph(std::uint8_t const dots)
    {
      // Every braille pattern is U+2800 plus its dot bits, a three-byte UTF-8
      // sequence whose lead byte is fixed.
      constexpr std::uint8_t kLeadByte = 0xE2;
      constexpr std::uint8_t kSecondByteBase = 0xA0;
      constexpr std::uint8_t kContinuationBase = 0x80;
      constexpr std::uint8_t kContinuationMask = 0x3F;
      constexpr std::int32_t kContinuationBits = 6;

      return std::string{static_cast<char>(kLeadByte),
                         static_cast<char>(kSecondByteBase | (dots >> kContinuationBits)),
                         static_cast<char>(kContinuationBase | (dots & kContinuationMask))};
    }

    void addSoulDot(SoulCanvas& canvas, SoulDot const dot, SoulDotSample const sample)
    {
      auto& cell = canvas[static_cast<std::size_t>(dot.column / 2)];
      auto const wasEmpty = cell.dots == 0;
      cell.dots |= brailleDotBit(dot);
      cell.headDistance = wasEmpty ? sample.headDistance : std::min(cell.headDistance, sample.headDistance);
      cell.coverage = wasEmpty ? sample.coverage : std::max(cell.coverage, sample.coverage);
    }

    std::optional<SoulDotSample> soulArcSample(double const distance, double const center, double const length)
    {
      auto const halfPerimeter = kSoulRingPerimeter / 2.0;
      auto offset = std::fmod(distance - center + halfPerimeter, kSoulRingPerimeter);

      if (offset < 0.0)
      {
        offset += kSoulRingPerimeter;
      }

      offset -= halfPerimeter;
      auto const halfLength = length / 2.0;
      auto const coverage =
        std::clamp(std::min(halfLength - offset, offset + halfLength) + kSoulDotDrawnCoverage, 0.0, 1.0);

      if (coverage < kSoulDotDrawnCoverage)
      {
        return std::nullopt;
      }

      // Clockwise travel puts the head at the far end of increasing distance.
      return SoulDotSample{.coverage = coverage, .headDistance = std::clamp((halfLength - offset) / length, 0.0, 1.0)};
    }

    SoulCanvas soulArcCanvas(double const rotationRadians, double const breath)
    {
      auto const length = kSoulArcExhaleLength + ((kSoulArcInhaleLength - kSoulArcExhaleLength) * breath);
      auto const center = kSoulArcOrigin + (rotationRadians / kFullTurnRadians * kSoulRingPerimeter);
      auto canvas = SoulCanvas{};
      auto ringSamples = std::array<std::optional<SoulDotSample>, kSoulRingDots.size()>{};

      for (std::size_t index = 0; index < kSoulRingDots.size(); ++index)
      {
        auto const optSample = soulArcSample(kSoulRingDistances[index], center, length);
        ringSamples[index] = optSample;

        if (optSample)
        {
          addSoulDot(canvas, kSoulRingDots[index], *optSample);
        }
      }

      for (auto const& inner : kSoulInnerDots)
      {
        if (auto const& optSample = ringSamples[inner.ringIndex]; optSample && optSample->coverage >= 1.0)
        {
          addSoulDot(canvas, inner.dot, *optSample);
        }
      }

      return canvas;
    }

    SoulCanvas dormantSoulCanvas()
    {
      auto canvas = SoulCanvas{};
      addSoulDot(canvas, {.column = 2, .row = 1}, {.coverage = 1.0, .headDistance = 1.0});
      return canvas;
    }

    // Errors keep the dormant aura, so the failed state reads as a ring broken
    // at its top rather than as a clipped-signal alarm.
    SoulCanvas brokenSoulCanvas()
    {
      constexpr std::size_t kGapBegin = 1;
      constexpr std::size_t kGapEnd = 3;
      auto canvas = SoulCanvas{};

      for (std::size_t index = 0; index < kSoulRingDots.size(); ++index)
      {
        if (index < kGapBegin || index >= kGapEnd)
        {
          addSoulDot(canvas, kSoulRingDots[index], {.coverage = 1.0, .headDistance = 1.0});
        }
      }

      return canvas;
    }

    double soulTransientRotation(std::chrono::milliseconds const elapsed)
    {
      auto const clamped = std::max(0.0, std::chrono::duration<double>{elapsed}.count());
      auto const cycleElapsed = std::fmod(clamped, kSoulTransientPulsePeriod.count());
      return kFullTurnRadians * cycleElapsed / kSoulTransientPulsePeriod.count();
    }

    SoulCanvas soulCanvas(audio::Transport const transport,
                          uimodel::AobusSoulMotionFrame const& motion,
                          std::chrono::milliseconds const transientElapsed)
    {
      switch (transport)
      {
        case audio::Transport::Playing:
        case audio::Transport::Paused: return soulArcCanvas(motion.rotationRadians, motion.breath);
        case audio::Transport::Opening:
        case audio::Transport::Buffering:
        case audio::Transport::Seeking:
          return soulArcCanvas(
            soulTransientRotation(transientElapsed), uimodel::aobusSoulMotionAt(transientElapsed).breath);
        case audio::Transport::Error: return brokenSoulCanvas();
        case audio::Transport::Idle:
        case audio::Transport::Stopping: break;
      }

      return dormantSoulCanvas();
    }

    double soulEdgeLuminance(double const coverage)
    {
      auto const fadeProgress = (coverage - kSoulDotDrawnCoverage) / (1.0 - kSoulDotDrawnCoverage);
      return kSoulEdgeLuminanceFloor + ((1.0 - kSoulEdgeLuminanceFloor) * std::clamp(fadeProgress, 0.0, 1.0));
    }

    uimodel::AobusSoulRgb soulCometColor(uimodel::AobusSoulGradientColors const& gradientColors,
                                         double const headDistance)
    {
      if (headDistance >= uimodel::kAobusSoulCoreGradientStop)
      {
        return gradientColors.body;
      }

      return uimodel::aobusSoulMixRgb(
        gradientColors.core, gradientColors.body, headDistance / uimodel::kAobusSoulCoreGradientStop);
    }

    uimodel::AobusSoulRgb soulCellColor(audio::Transport const transport,
                                        uimodel::AobusSoulVisualFrame const& visual,
                                        SoulCell const& cell)
    {
      switch (transport)
      {
        case audio::Transport::Playing:
        case audio::Transport::Paused:
          return uimodel::aobusSoulScaleRgb(soulCometColor(visual.gradientColors, cell.headDistance),
                                            visual.motion.luminance * soulEdgeLuminance(cell.coverage));
        case audio::Transport::Opening:
        case audio::Transport::Buffering:
        case audio::Transport::Seeking:
          return uimodel::aobusSoulScaleRgb(
            visual.gradientColors.body, kTransientSoulLuminance * soulEdgeLuminance(cell.coverage));
        case audio::Transport::Error:
        case audio::Transport::Idle:
        case audio::Transport::Stopping: break;
      }

      // Stopped playback and errors keep a dim cyan core, matching the GTK soul.
      return uimodel::aobusSoulScaleRgb(visual.gradientColors.body, kDormantSoulLuminance);
    }
  } // namespace

  std::string soulTitleText(audio::Transport const transport,
                            uimodel::AobusSoulMotionFrame const& motion,
                            std::chrono::milliseconds const transientElapsed)
  {
    auto result = std::string{};

    // Empty cells stay braille blanks, which preserve the canvas when terminals
    // trim title whitespace.
    for (auto const& cell : soulCanvas(transport, motion, transientElapsed))
    {
      result.append(brailleGlyph(cell.dots));
    }

    return result;
  }

  ftxui::Element soulButtonElement(audio::Transport const transport,
                                   uimodel::AobusSoulVisualFrame const& visual,
                                   std::chrono::milliseconds const transientElapsed)
  {
    auto cells = ftxui::Elements{};
    cells.reserve(static_cast<std::size_t>(kSoulGlyphColumns));

    for (auto const& cell : soulCanvas(transport, visual.motion, transientElapsed))
    {
      if (cell.dots == 0)
      {
        cells.push_back(ftxui::text(" "));
        continue;
      }

      auto const color = soulCellColor(transport, visual, cell);
      cells.push_back(ftxui::text(brailleGlyph(cell.dots)) |
                      ftxui::color(ftxui::Color::RGB(color.red, color.green, color.blue)) | ftxui::bold);
    }

    return ftxui::hbox(std::move(cells)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSoulGlyphColumns);
  }

  void SoulAnimationClock::update(rt::PlaybackTransportSnapshot const& state,
                                  bool const reducedMotion,
                                  uimodel::FrameClock::TimePoint const frameTime) noexcept
  {
    _animation.setMotionMode(uimodel::aobusSoulMotionMode(state.transport));
    _animation.setAura(
      uimodel::aobusSoulAuraRgb(uimodel::resolveSoulAura(state.transport, state.ready, state.quality)));

    if (reducedMotion || !_animation.needsFrames())
    {
      _animation.settle();
      _optPreviousFrameTime.reset();
      return;
    }

    if (_optPreviousFrameTime)
    {
      _animation.advance(frameTime - *_optPreviousFrameTime);
    }

    // The frame that settles the Soul is the last one requested, so the next
    // transition starts a fresh interval instead of spanning the idle gap.
    _optPreviousFrameTime =
      _animation.needsFrames() ? std::optional{frameTime} : std::optional<uimodel::FrameClock::TimePoint>{};
  }

  bool SoulAnimationClock::isSettling() const noexcept
  {
    return _animation.needsFrames() && _animation.motionMode() != uimodel::AobusSoulMotionMode::Animating;
  }

  uimodel::AobusSoulAnimationState const& SoulAnimationClock::animation() const noexcept
  {
    return _animation;
  }
} // namespace ao::tui
