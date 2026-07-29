// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SoulButton.h"

#include "TextCell.h"
#include <ao/audio/Transport.h>
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
#include <string_view>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kSoulGlyphColumns = 3;
    constexpr double kSoulTransientPulseDivisor = 4.0;
    constexpr double kSoulNarrowBreathThreshold = 1.0 / 3.0;
    constexpr double kSoulWideBreathThreshold = 2.0 / 3.0;
    constexpr double kTerminalCellCenterScale = 2.0;
    constexpr double kTerminalCellCenterOffset = 0.5;
    constexpr double kSoulGradientDiagonalScale = 4.0;
    constexpr double kTransientSoulLuminance = 0.9;
    constexpr double kDormantSoulLuminance = 0.5;
    constexpr auto kSoulTransientPulsePeriod =
      std::chrono::duration<double>{uimodel::kAobusSoulBreathingPeriod.count() / kSoulTransientPulseDivisor};
    using SoulFrame = std::array<std::string_view, kSoulGlyphColumns>;
    constexpr std::size_t kSoulArcFrameCount = 12;
    using SoulArcFrames = std::array<SoulFrame, kSoulArcFrameCount>;
    enum class SoulArcBreadth : std::uint8_t
    {
      Narrow = 0,
      Balanced,
      Wide,
    };

    constexpr auto kPlayingSoulArcFrames = std::to_array<SoulArcFrames>({
      SoulArcFrames{SoulFrame{" ", " ", "⠰"},
                    SoulFrame{" ", " ", "⠳"},
                    SoulFrame{" ", "⠉", "⠓"},
                    SoulFrame{"⠈", "⠉", "⠁"},
                    SoulFrame{"⠚", "⠉", " "},
                    SoulFrame{"⠞", " ", " "},
                    SoulFrame{"⠆", " ", " "},
                    SoulFrame{"⢦", " ", " "},
                    SoulFrame{"⢤", "⣀", " "},
                    SoulFrame{"⢀", "⣀", "⡀"},
                    SoulFrame{" ", "⣀", "⡤"},
                    SoulFrame{" ", " ", "⡴"}},
      SoulArcFrames{SoulFrame{" ", " ", "⡷"},
                    SoulFrame{" ", "⠈", "⠳"},
                    SoulFrame{" ", "⠉", "⠓"},
                    SoulFrame{"⠈", "⠉", "⠁"},
                    SoulFrame{"⠚", "⠉", " "},
                    SoulFrame{"⠞", "⠁", " "},
                    SoulFrame{"⢾", " ", " "},
                    SoulFrame{"⢦", "⡀", " "},
                    SoulFrame{"⢤", "⣀", " "},
                    SoulFrame{"⢀", "⣀", "⡀"},
                    SoulFrame{" ", "⣀", "⡤"},
                    SoulFrame{" ", "⢀", "⡴"}},
      SoulArcFrames{SoulFrame{" ", " ", "⡷"},
                    SoulFrame{" ", "⠉", "⠳"},
                    SoulFrame{"⠈", "⠉", "⠳"},
                    SoulFrame{"⠚", "⠉", "⠓"},
                    SoulFrame{"⠞", "⠉", "⠁"},
                    SoulFrame{"⠞", "⠉", " "},
                    SoulFrame{"⢾", " ", " "},
                    SoulFrame{"⢦", "⣀", " "},
                    SoulFrame{"⢦", "⣀", "⡀"},
                    SoulFrame{"⢤", "⣀", "⡤"},
                    SoulFrame{"⢀", "⣀", "⡴"},
                    SoulFrame{" ", "⣀", "⡴"}},
    });
    constexpr auto kDormantSoulFrame = SoulFrame{" ", "⠂", " "};

    ftxui::Element fixedText(std::string_view const value,
                             std::int32_t const columns,
                             CellAlignment const alignment = CellAlignment::Left)
    {
      return ftxui::text(fitCellText(value, columns, alignment)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, columns);
    }

    // The soul button keeps the GTK AobusSoul color and timing recipe, but uses a
    // three-cell braille canvas for the terminal shape. Only a partial abstract
    // arc is drawn; it rotates inside those cells while the GTK stroke-width
    // breathing phase makes the arc narrow, balanced, or wide. The GTK cyan-core
    // gradient is projected onto the visible arc.
    using SoulRgb = uimodel::AobusSoulRgb;

    constexpr double kSoulGradientCenterX = 2.5;
    constexpr double kSoulGradientRadiusX = 2.2;

    std::size_t soulFrameIndex(std::chrono::milliseconds const elapsed,
                               std::chrono::duration<double> const period,
                               std::size_t const frameCount)
    {
      auto const clamped = std::max(0.0, std::chrono::duration<double>{elapsed}.count());
      auto const cycleElapsed = std::fmod(clamped, period.count());
      auto const frameIndex =
        static_cast<std::size_t>((cycleElapsed * static_cast<double>(frameCount)) / period.count());
      return std::min(frameIndex, frameCount - 1);
    }

    SoulArcBreadth soulArcBreadth(double const breath)
    {
      if (breath < kSoulNarrowBreathThreshold)
      {
        return SoulArcBreadth::Narrow;
      }

      if (breath > kSoulWideBreathThreshold)
      {
        return SoulArcBreadth::Wide;
      }

      return SoulArcBreadth::Balanced;
    }

    SoulFrame const& soulArcFrame(uimodel::AobusSoulMotionFrame const& motion)
    {
      auto const& frames = kPlayingSoulArcFrames[static_cast<std::size_t>(soulArcBreadth(motion.breath))];
      auto const normalizedRotation = std::fmod(std::max(0.0, motion.rotationRadians), 2.0 * std::numbers::pi);
      auto const frameIndex =
        static_cast<std::size_t>((normalizedRotation * static_cast<double>(frames.size())) / (2.0 * std::numbers::pi));
      return frames[std::min(frameIndex, frames.size() - 1)];
    }

    SoulFrame const& transientSoulArcFrame(std::chrono::milliseconds const elapsed,
                                           std::chrono::duration<double> const period)
    {
      auto const motion = uimodel::aobusSoulMotionAt(elapsed);
      auto const& frames = kPlayingSoulArcFrames[static_cast<std::size_t>(soulArcBreadth(motion.breath))];
      return frames[soulFrameIndex(elapsed, period, frames.size())];
    }

    struct SoulFrameSpec final
    {
      SoulFrame frame{};
      std::optional<double> optRotation{};
      uimodel::AobusSoulGradientColors gradientColors{};
      double luminance = 1.0;
    };

    double soulGradientPosition(std::size_t const cellIndex, double const rotation)
    {
      auto const cellCenterX = (static_cast<double>(cellIndex) * kTerminalCellCenterScale) + kTerminalCellCenterOffset;
      double const normalizedX = (cellCenterX - kSoulGradientCenterX) / kSoulGradientRadiusX;
      constexpr double kNormalizedY = 0.0;
      auto const localX = (normalizedX * std::cos(rotation)) + (kNormalizedY * std::sin(rotation));
      auto const localY = (-normalizedX * std::sin(rotation)) + (kNormalizedY * std::cos(rotation));

      // GTK's Soul gradient runs from (+r,+r) cyan to (-r,-r) aura before the
      // drawing transform rotates it. This is the same diagonal gradient projected
      // onto the three terminal cells.
      return std::clamp(kTerminalCellCenterOffset - ((localX + localY) / kSoulGradientDiagonalScale), 0.0, 1.0);
    }

    SoulRgb soulGradientColor(uimodel::AobusSoulGradientColors const& gradientColors,
                              std::optional<double> const optRotation,
                              std::size_t const cellIndex,
                              double const luminance)
    {
      auto const shiftedCyan = gradientColors.core;
      auto const shiftedAura = gradientColors.body;

      if (!optRotation)
      {
        return uimodel::aobusSoulScaleRgb(shiftedAura, luminance);
      }

      auto const gradientPosition = soulGradientPosition(cellIndex, *optRotation);

      if (gradientPosition >= uimodel::kAobusSoulCoreGradientStop)
      {
        return uimodel::aobusSoulScaleRgb(shiftedAura, luminance);
      }

      return uimodel::aobusSoulScaleRgb(
        uimodel::aobusSoulMixRgb(shiftedCyan, shiftedAura, gradientPosition / uimodel::kAobusSoulCoreGradientStop),
        luminance);
    }

    ftxui::Element soulFrameElement(SoulFrameSpec const& spec)
    {
      auto cells = ftxui::Elements{};
      cells.reserve(spec.frame.size());

      for (std::size_t cellIndex = 0; cellIndex < spec.frame.size(); ++cellIndex)
      {
        auto const color = soulGradientColor(spec.gradientColors, spec.optRotation, cellIndex, spec.luminance);
        auto cellPtr = ftxui::text(std::string{spec.frame[cellIndex]});

        if (spec.frame[cellIndex] != " ")
        {
          cellPtr = std::move(cellPtr) | ftxui::color(ftxui::Color::RGB(color.red, color.green, color.blue));

          if (cellIndex == 1 || spec.optRotation)
          {
            cellPtr = std::move(cellPtr) | ftxui::bold;
          }
        }

        cells.push_back(std::move(cellPtr));
      }

      return ftxui::hbox(std::move(cells)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSoulGlyphColumns);
    }
  } // namespace

  ftxui::Element soulButtonElement(audio::Transport const transport,
                                   uimodel::AobusSoulVisualFrame const& visual,
                                   std::chrono::milliseconds const transientElapsed)
  {
    switch (transport)
    {
      case audio::Transport::Playing:
      case audio::Transport::Paused:
        return soulFrameElement({.frame = soulArcFrame(visual.motion),
                                 .optRotation = visual.motion.rotationRadians,
                                 .gradientColors = visual.gradientColors,
                                 .luminance = visual.motion.luminance});
      case audio::Transport::Opening:
      case audio::Transport::Buffering:
      case audio::Transport::Seeking:
      {
        return soulFrameElement({.frame = transientSoulArcFrame(transientElapsed, kSoulTransientPulsePeriod),
                                 .gradientColors = visual.gradientColors,
                                 .luminance = kTransientSoulLuminance});
      }
      case audio::Transport::Error:
        return fixedText("!!!", kSoulGlyphColumns) |
               ftxui::color(ftxui::Color::RGB(
                 uimodel::kAobusSoulBurning.red, uimodel::kAobusSoulBurning.green, uimodel::kAobusSoulBurning.blue)) |
               ftxui::bold;
      case audio::Transport::Idle:
      case audio::Transport::Stopping: break;
    }

    // Dormant: stopped playback keeps a dim cyan core, matching the GTK soul.
    return soulFrameElement(
      {.frame = kDormantSoulFrame, .gradientColors = visual.gradientColors, .luminance = kDormantSoulLuminance});
  }
} // namespace ao::tui
