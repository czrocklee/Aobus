// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/SoulButton.h"

#include <ao/audio/Transport.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <array>
#include <chrono>
#include <string>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Screen renderScreen(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(3), ftxui::Dimension::Fixed(1));
      ftxui::Render(screen, elementPtr);
      return screen;
    }

    std::array<std::string, 3> soulButtonCells(ftxui::Screen const& screen)
    {
      return {screen.PixelAt(0, 0).character, screen.PixelAt(1, 0).character, screen.PixelAt(2, 0).character};
    }
  } // namespace

  TEST_CASE("SoulButton - playing state rotates a partial arc across the three-cell canvas", "[tui][unit][soul]")
  {
    auto renderAt = [](std::chrono::milliseconds const animationElapsed)
    {
      return renderScreen(soulButtonElement(audio::Transport::Playing,
                                            uimodel::aobusSoulVisualAt(uimodel::kAobusSoulRadiant, animationElapsed),
                                            animationElapsed));
    };

    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{0})) == std::array<std::string, 3>{" ", " ", "⡷"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{700})) == std::array<std::string, 3>{" ", "⠉", "⠳"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{1390})) == std::array<std::string, 3>{"⠈", "⠉", "⠳"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{2080})) == std::array<std::string, 3>{"⠚", "⠉", "⠓"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{2770})) == std::array<std::string, 3>{"⠚", "⠉", " "});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{3460})) == std::array<std::string, 3>{"⠞", " ", " "});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{4150})) == std::array<std::string, 3>{"⠆", " ", " "});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{4840})) == std::array<std::string, 3>{"⢦", " ", " "});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{5530})) == std::array<std::string, 3>{"⢦", "⣀", "⡀"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{6220})) == std::array<std::string, 3>{"⢤", "⣀", "⡤"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{6910})) == std::array<std::string, 3>{"⢀", "⣀", "⡴"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{7600})) == std::array<std::string, 3>{" ", "⢀", "⡴"});
  }

  TEST_CASE("SoulButton - playing state breathes between narrow and wide arc spans", "[tui][unit][soul]")
  {
    auto renderAt = [](std::chrono::milliseconds const animationElapsed)
    {
      return renderScreen(soulButtonElement(audio::Transport::Playing,
                                            uimodel::aobusSoulVisualAt(uimodel::kAobusSoulRadiant, animationElapsed),
                                            animationElapsed));
    };

    // These timestamps have the same rotation frame. Only GTK's stroke-width
    // breathing phase changes the TUI arc breadth.
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{700})) == std::array<std::string, 3>{" ", "⠉", "⠳"});
    CHECK(soulButtonCells(renderAt(std::chrono::milliseconds{8983})) == std::array<std::string, 3>{" ", " ", "⠳"});
  }

  TEST_CASE("SoulButton - paused state preserves the sampled arc while quality color changes", "[tui][unit][soul]")
  {
    auto const frozenVisual = uimodel::aobusSoulVisualAt(uimodel::kAobusSoulRadiant, std::chrono::milliseconds{2080});
    auto const renderAt = [&frozenVisual](std::chrono::milliseconds const transientElapsed)
    { return renderScreen(soulButtonElement(audio::Transport::Paused, frozenVisual, transientElapsed)); };

    auto const early = renderAt(std::chrono::milliseconds{0});
    auto const late = renderAt(std::chrono::milliseconds{5120});

    CHECK(soulButtonCells(early) == std::array<std::string, 3>{"⠚", "⠉", "⠓"});
    CHECK(soulButtonCells(late) == soulButtonCells(early));
    CHECK(early.PixelAt(1, 0).foreground_color == late.PixelAt(1, 0).foreground_color);

    auto const recoloredVisual = uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulTurbulent, frozenVisual.motion);
    auto const recolored =
      renderScreen(soulButtonElement(audio::Transport::Paused, recoloredVisual, std::chrono::milliseconds{9000}));
    CHECK(soulButtonCells(recolored) == soulButtonCells(early));
    CHECK_FALSE(recolored.PixelAt(1, 0).foreground_color == early.PixelAt(1, 0).foreground_color);
  }

  TEST_CASE("SoulButton - playing aura projects cyan core onto the rotating arc edge", "[tui][unit][soul]")
  {
    auto renderAt = [](std::chrono::milliseconds const animationElapsed)
    {
      return renderScreen(soulButtonElement(audio::Transport::Playing,
                                            uimodel::aobusSoulVisualAt(uimodel::kAobusSoulRadiant, animationElapsed),
                                            animationElapsed));
    };

    // The GTK cyan-core side of the gradient moves over the same three-cell
    // canvas as the visible arc instead of becoming a separate status dot.
    auto const coreLeft = renderAt(std::chrono::milliseconds{2600});

    CHECK_FALSE(coreLeft.PixelAt(0, 0).foreground_color == coreLeft.PixelAt(1, 0).foreground_color);

    // Later, it has orbited to the opposite side of the arc.
    auto const coreRight = renderAt(std::chrono::milliseconds{7600});

    CHECK_FALSE(coreRight.PixelAt(2, 0).foreground_color == coreRight.PixelAt(1, 0).foreground_color);
  }
} // namespace ao::tui::test
