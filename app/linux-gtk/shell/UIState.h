// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  constexpr std::int32_t kDefaultWindowWidth = 989;
  constexpr std::int32_t kDefaultWindowHeight = 801;
  constexpr std::int32_t kDefaultPanedPosition = 330;

  struct WindowState final
  {
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;
    std::int32_t panedPosition = kDefaultPanedPosition;
  };

  struct TrackViewState final
  {
    std::vector<std::string> columnOrder;
    std::vector<std::string> hiddenColumns;
    std::map<std::string, std::int32_t, std::less<>> columnWidths;
  };
}
