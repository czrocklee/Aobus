// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::tui
{
  inline constexpr std::int32_t kMinimumNavigationColumns = 18;
  inline constexpr std::int32_t kMinimumDetailColumns = 24;
  inline constexpr std::int32_t kMinimumTrackColumns = 72;

  /// Preferred framed pane widths in terminal cells; zero retains automatic sizing.
  struct PanelWidths final
  {
    std::int32_t navigation = 0;
    std::int32_t detail = 0;
    bool operator==(PanelWidths const&) const = default;
  };
} // namespace ao::tui
