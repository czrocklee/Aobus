// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "runtime/TrackField.h"

#include <cstdint>
#include <map>
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
  };

  struct ColumnState final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t width = -1;

    bool operator==(ColumnState const&) const = default;
  };

  struct ColumnLayoutState final
  {
    std::map<ao::ListId, std::vector<ColumnState>> listLayouts;
  };
} // namespace ao::gtk
