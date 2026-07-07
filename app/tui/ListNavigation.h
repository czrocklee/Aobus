// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ftxui/component/event.hpp>

#include <cstdint>

namespace ao::tui
{
  enum class ListNavigationAction : std::uint8_t
  {
    None,
    Previous,
    Next,
    PagePrevious,
    PageNext,
    First,
    Last,
  };

  struct ListNavigationDecision final
  {
    ListNavigationAction action = ListNavigationAction::None;
    std::int32_t delta = 0;
  };

  ListNavigationDecision listNavigationDecision(ftxui::Event const& event);

  template<typename MoveSelection>
  bool handleListNavigation(ftxui::Event const& event, MoveSelection moveSelection)
  {
    auto const decision = listNavigationDecision(event);

    if (decision.action == ListNavigationAction::None)
    {
      return false;
    }

    moveSelection(decision.delta);
    return true;
  }
} // namespace ao::tui
