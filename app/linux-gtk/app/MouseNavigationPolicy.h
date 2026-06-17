// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>

namespace ao::gtk
{
  /**
   * @brief Workspace history navigation direction requested by a pointer button.
   */
  enum class WorkspaceNavigation : std::uint8_t
  {
    Back,
    Forward,
  };

  /**
   * @brief Maps a GTK pointer button to a workspace history navigation, if any.
   *
   * The conventional mouse "back"/"forward" thumb buttons are 8 and 9. Any other
   * button yields no navigation.
   */
  std::optional<WorkspaceNavigation> mouseButtonNavigation(std::int32_t button);
} // namespace ao::gtk
