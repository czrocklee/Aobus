// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PanelWidths.h"

#include <cstdint>

namespace ao::tui
{
  struct NavigationGeometry;

  enum class PanelDivider : std::uint8_t
  {
    Navigation,
    Detail,
  };

  /// A resize snapshot shared by keyboard steps and pointer drags.
  class PanelResize final
  {
  public:
    PanelResize(PanelWidths widths, NavigationGeometry const& geometry, PanelDivider divider) noexcept;
    PanelDivider divider() const noexcept { return _divider; }
    /// Moves the divider by signed screen columns, preserving the other preference.
    PanelWidths moved(std::int32_t columns) const noexcept;

  private:
    PanelWidths _widths;
    PanelDivider _divider;
    std::int32_t _initialColumns;
    std::int32_t _minimumColumns;
    std::int32_t _maximumColumns;
  };
} // namespace ao::tui
