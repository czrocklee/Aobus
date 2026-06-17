// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MouseNavigationPolicy.h"

#include <cstdint>
#include <optional>

namespace ao::gtk
{
  namespace
  {
    constexpr std::int32_t kMouseBackButton = 8;
    constexpr std::int32_t kMouseForwardButton = 9;
  } // namespace

  std::optional<WorkspaceNavigation> mouseButtonNavigation(std::int32_t button)
  {
    switch (button)
    {
      case kMouseBackButton: return WorkspaceNavigation::Back;
      case kMouseForwardButton: return WorkspaceNavigation::Forward;
      default: return std::nullopt;
    }
  }
} // namespace ao::gtk
