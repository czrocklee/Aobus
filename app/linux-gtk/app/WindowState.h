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
} // namespace ao::gtk
