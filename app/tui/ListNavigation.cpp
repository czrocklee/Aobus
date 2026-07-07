// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListNavigation.h"

#include <ftxui/component/event.hpp>

#include <cstdint>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kPageSelectionDelta = 10;
    constexpr std::int32_t kBoundarySelectionDelta = 1'000'000;
  } // namespace

  ListNavigationDecision listNavigationDecision(ftxui::Event const& event)
  {
    if (event == ftxui::Event::ArrowUp)
    {
      return ListNavigationDecision{.action = ListNavigationAction::Previous, .delta = -1};
    }

    if (event == ftxui::Event::ArrowDown)
    {
      return ListNavigationDecision{.action = ListNavigationAction::Next, .delta = 1};
    }

    if (event == ftxui::Event::PageUp)
    {
      return ListNavigationDecision{.action = ListNavigationAction::PagePrevious, .delta = -kPageSelectionDelta};
    }

    if (event == ftxui::Event::PageDown)
    {
      return ListNavigationDecision{.action = ListNavigationAction::PageNext, .delta = kPageSelectionDelta};
    }

    if (event == ftxui::Event::Home)
    {
      return ListNavigationDecision{.action = ListNavigationAction::First, .delta = -kBoundarySelectionDelta};
    }

    if (event == ftxui::Event::End)
    {
      return ListNavigationDecision{.action = ListNavigationAction::Last, .delta = kBoundarySelectionDelta};
    }

    return {};
  }
} // namespace ao::tui
