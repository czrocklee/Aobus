// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/SoulButton.h"

#include <ao/audio/Transport.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>

namespace ao::tui::test
{
  namespace
  {
    using SoulCells = std::array<std::string, 3>;

    ftxui::Screen renderScreen(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(3), ftxui::Dimension::Fixed(1));
      ftxui::Render(screen, elementPtr);
      return screen;
    }

    SoulCells soulButtonCells(ftxui::Screen const& screen)
    {
      return {screen.PixelAt(0, 0).character, screen.PixelAt(1, 0).character, screen.PixelAt(2, 0).character};
    }

    // A frame with fixed luminance and hue, so colors differ only through the
    // arc geometry under test.
    uimodel::AobusSoulVisualFrame radiantVisualAt(double const rotationDegrees, double const breath)
    {
      auto const motion = uimodel::AobusSoulMotionFrame{.breath = breath,
                                                        .rotationRadians = rotationDegrees * std::numbers::pi / 180.0,
                                                        .rotationDegrees = rotationDegrees,
                                                        .luminance = 1.0,
                                                        .hueShiftDegrees = 0.0};
      return uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulRadiant, motion);
    }

    ftxui::Screen renderPlaying(uimodel::AobusSoulVisualFrame const& visual)
    {
      return renderScreen(soulButtonElement(audio::Transport::Playing, visual, std::chrono::milliseconds{0}));
    }

    std::uint8_t brailleDots(std::string const& glyph)
    {
      if (glyph.size() != 3)
      {
        return 0;
      }

      auto const lowBits = static_cast<std::uint8_t>(glyph[2]) & 0x3FU;
      auto const highBits = static_cast<std::uint8_t>(glyph[1]) & 0x03U;
      return static_cast<std::uint8_t>((highBits << 6U) | lowBits);
    }

    std::array<std::uint8_t, 3> soulButtonDots(ftxui::Screen const& screen)
    {
      auto const cells = soulButtonCells(screen);
      return {brailleDots(cells[0]), brailleDots(cells[1]), brailleDots(cells[2])};
    }

    ftxui::Color rgb(uimodel::AobusSoulRgb const color)
    {
      return ftxui::Color::RGB(color.red, color.green, color.blue);
    }
  } // namespace

  TEST_CASE("SoulButton - playing arc travels clockwise from the right side", "[tui][unit][soul]")
  {
    CHECK(soulButtonCells(renderPlaying(radiantVisualAt(0.0, 0.5))) == SoulCells{" ", "⢈", "⡷"});
    CHECK(soulButtonCells(renderPlaying(radiantVisualAt(90.0, 0.5))) == SoulCells{"⢄", "⣀", "⡠"});
    CHECK(soulButtonCells(renderPlaying(radiantVisualAt(180.0, 0.5))) == SoulCells{"⢾", "⡁", " "});
    CHECK(soulButtonCells(renderPlaying(radiantVisualAt(270.0, 0.5))) == SoulCells{"⠊", "⠉", "⠑"});
  }

  TEST_CASE("SoulButton - playing arc advances at most one dot per end between close samples", "[tui][unit][soul]")
  {
    constexpr auto kSampleInterval = std::chrono::milliseconds{50};
    auto const sampleCount = static_cast<std::int32_t>(2.0 * uimodel::kAobusSoulRotationPeriod /
                                                       std::chrono::duration<double>{kSampleInterval});
    auto previous = soulButtonDots(renderPlaying(uimodel::aobusSoulVisualFrame(
      uimodel::kAobusSoulRadiant, uimodel::aobusSoulMotionAt(std::chrono::milliseconds{0}))));

    for (std::int32_t sample = 1; sample <= sampleCount; ++sample)
    {
      auto const elapsed = kSampleInterval * sample;
      auto const current = soulButtonDots(
        renderPlaying(uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulRadiant, uimodel::aobusSoulMotionAt(elapsed))));
      std::int32_t changedDots = 0;

      for (std::size_t cell = 0; cell < current.size(); ++cell)
      {
        changedDots += std::popcount(static_cast<std::uint8_t>(previous[cell] ^ current[cell]));
      }

      INFO("elapsed ms: " << elapsed.count());
      CHECK(changedDots <= 2);
      previous = current;
    }
  }

  TEST_CASE("SoulButton - inhale lengthens the arc around the same rotation", "[tui][unit][soul]")
  {
    auto const exhale = soulButtonDots(renderPlaying(radiantVisualAt(90.0, 0.0)));
    auto const inhale = soulButtonDots(renderPlaying(radiantVisualAt(90.0, 1.0)));
    std::int32_t exhaleDotCount = 0;
    std::int32_t inhaleDotCount = 0;

    for (std::size_t cell = 0; cell < exhale.size(); ++cell)
    {
      CHECK((exhale[cell] & inhale[cell]) == exhale[cell]);
      exhaleDotCount += std::popcount(exhale[cell]);
      inhaleDotCount += std::popcount(inhale[cell]);
    }

    CHECK(inhaleDotCount > exhaleDotCount);
  }

  TEST_CASE("SoulButton - arc head carries the cyan core and its tail carries the aura", "[tui][unit][soul]")
  {
    // At a quarter turn the clockwise arc runs right to left along the bottom,
    // so its head is in the left cell and its tail in the right cell.
    auto const screen = renderPlaying(radiantVisualAt(90.0, 1.0));

    REQUIRE(soulButtonCells(screen) == SoulCells{"⢤", "⣀", "⡤"});
    CHECK(screen.PixelAt(2, 0).foreground_color == rgb(uimodel::kAobusSoulRadiant));
    CHECK_FALSE(screen.PixelAt(0, 0).foreground_color == screen.PixelAt(2, 0).foreground_color);
  }

  TEST_CASE("SoulButton - tail cell dims before its last dot leaves", "[tui][unit][soul]")
  {
    auto const covered = renderPlaying(radiantVisualAt(112.0, 0.5));
    auto const leaving = renderPlaying(radiantVisualAt(124.0, 0.5));

    REQUIRE(covered.PixelAt(2, 0).character == "⡀");
    REQUIRE(leaving.PixelAt(2, 0).character == "⡀");
    CHECK(covered.PixelAt(2, 0).foreground_color == rgb(uimodel::kAobusSoulRadiant));
    CHECK_FALSE(leaving.PixelAt(2, 0).foreground_color == covered.PixelAt(2, 0).foreground_color);
  }

  TEST_CASE("SoulButton - paused state preserves the sampled arc while quality color changes", "[tui][unit][soul]")
  {
    auto const frozenVisual = uimodel::aobusSoulVisualFrame(
      uimodel::kAobusSoulRadiant, uimodel::aobusSoulMotionAt(std::chrono::milliseconds{2080}));
    auto const renderAt = [&frozenVisual](std::chrono::milliseconds const transientElapsed)
    { return renderScreen(soulButtonElement(audio::Transport::Paused, frozenVisual, transientElapsed)); };

    auto const early = renderAt(std::chrono::milliseconds{0});
    auto const late = renderAt(std::chrono::milliseconds{5120});

    REQUIRE(early.PixelAt(1, 0).character != " ");
    CHECK(soulButtonCells(late) == soulButtonCells(early));
    CHECK(early.PixelAt(1, 0).foreground_color == late.PixelAt(1, 0).foreground_color);

    auto const recoloredVisual = uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulTurbulent, frozenVisual.motion);
    auto const recolored =
      renderScreen(soulButtonElement(audio::Transport::Paused, recoloredVisual, std::chrono::milliseconds{9000}));
    CHECK(soulButtonCells(recolored) == soulButtonCells(early));
    CHECK_FALSE(recolored.PixelAt(1, 0).foreground_color == early.PixelAt(1, 0).foreground_color);
  }

  TEST_CASE("SoulButton - error shows a broken ring in the dormant color", "[tui][unit][soul]")
  {
    auto const dormantVisual = uimodel::aobusSoulVisualFrame(uimodel::aobusSoulAuraRgb(uimodel::SoulAura::Dormant), {});
    auto const error =
      renderScreen(soulButtonElement(audio::Transport::Error, dormantVisual, std::chrono::milliseconds{0}));
    auto const idle =
      renderScreen(soulButtonElement(audio::Transport::Idle, dormantVisual, std::chrono::milliseconds{0}));

    CHECK(soulButtonCells(error) == SoulCells{"⢎", "⣀", "⡱"});
    REQUIRE(soulButtonCells(idle) == SoulCells{" ", "⠂", " "});

    for (std::int32_t cell = 0; cell < 3; ++cell)
    {
      CHECK(error.PixelAt(cell, 0).foreground_color == idle.PixelAt(1, 0).foreground_color);
    }
  }

  TEST_CASE("SoulButton - title shares transport glyphs and keeps a fixed blank canvas", "[tui][unit][soul]")
  {
    auto const motion = uimodel::aobusSoulMotionAt(std::chrono::milliseconds{0});
    auto const button = renderScreen(soulButtonElement(
      audio::Transport::Playing, uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulRadiant, motion), {}));

    REQUIRE(soulButtonCells(button) == SoulCells{" ", "⢈", "⡷"});
    CHECK(soulTitleText(audio::Transport::Playing, motion, std::chrono::milliseconds{0}) == "⠀⢈⡷");
    CHECK(soulTitleText(audio::Transport::Paused, motion, std::chrono::milliseconds{9000}) == "⠀⢈⡷");
    CHECK(soulTitleText(audio::Transport::Idle, motion, std::chrono::milliseconds{0}) == "⠀⠂⠀");
    CHECK(soulTitleText(audio::Transport::Error, motion, std::chrono::milliseconds{0}) == "⢎⣀⡱");
    CHECK(soulTitleText(audio::Transport::Buffering, motion, std::chrono::milliseconds{0}) !=
          soulTitleText(audio::Transport::Buffering, motion, std::chrono::milliseconds{700}));
  }
} // namespace ao::tui::test
