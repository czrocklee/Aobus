// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::winui
{
  inline constexpr std::int32_t kDefaultWindowX = 80;
  inline constexpr std::int32_t kDefaultWindowY = 80;
  inline constexpr std::int32_t kDefaultWindowWidth = 1280;
  inline constexpr std::int32_t kDefaultWindowHeight = 800;

  /// The normal window bounds to restore, even when the window is maximized.
  struct WindowPlacement final
  {
    std::int32_t x = kDefaultWindowX;
    std::int32_t y = kDefaultWindowY;
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;

    friend bool operator==(WindowPlacement const&, WindowPlacement const&) = default;
  };
} // namespace ao::winui
