// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/ListNavigation.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstdint>

namespace ao::tui::test
{
  namespace
  {
    void checkDecision(ftxui::Event const& event, ListNavigationAction const action, std::int32_t const delta)
    {
      auto const decision = listNavigationDecision(event);

      CHECK(decision.action == action);
      CHECK(decision.delta == delta);
    }
  } // namespace

  TEST_CASE("listNavigationDecision maps navigation keys to stable deltas", "[tui][unit][navigation]")
  {
    checkDecision(ftxui::Event::ArrowUp, ListNavigationAction::Previous, -1);
    checkDecision(ftxui::Event::ArrowDown, ListNavigationAction::Next, 1);
    checkDecision(ftxui::Event::PageUp, ListNavigationAction::PagePrevious, -10);
    checkDecision(ftxui::Event::PageDown, ListNavigationAction::PageNext, 10);
    checkDecision(ftxui::Event::Home, ListNavigationAction::First, -1'000'000);
    checkDecision(ftxui::Event::End, ListNavigationAction::Last, 1'000'000);
    checkDecision(ftxui::Event::Character("x"), ListNavigationAction::None, 0);
  }

  TEST_CASE("handleListNavigation applies only recognized navigation decisions", "[tui][unit][navigation]")
  {
    std::int32_t appliedDelta = 0;

    CHECK(handleListNavigation(ftxui::Event::PageDown, [&](std::int32_t const delta) { appliedDelta = delta; }));
    CHECK(appliedDelta == 10);

    CHECK_FALSE(
      handleListNavigation(ftxui::Event::Character("x"), [&](std::int32_t const delta) { appliedDelta = delta; }));
    CHECK(appliedDelta == 10);
  }
} // namespace ao::tui::test
