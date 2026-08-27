// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk
{
  constexpr std::int32_t kDefaultWindowWidth = 989;
  constexpr std::int32_t kDefaultWindowHeight = 801;

  struct WindowState final
  {
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;
  };

  /**
   * @brief Fold one geometry checkpoint into the state that will be persisted.
   *
   * A maximized window reports its maximized extent, which is not the size the
   * user wants back on the next unmaximized start. A maximized snapshot
   * therefore records only the flag and leaves the last normal geometry of the
   * session in place. Zero extents are ignored for the same reason: they are
   * what an unrealized or hidden window reports, not a size the user chose.
   */
  void recordWindowGeometry(WindowState& state, WindowState const& snapshot) noexcept;
} // namespace ao::gtk
