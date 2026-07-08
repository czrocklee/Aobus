// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <string>

namespace ao::tui
{
  struct TrackSection final
  {
    std::size_t rowBegin = 0;
    std::size_t rowCount = 0;
    std::string primaryText{};
    std::string secondaryText{};
    std::string tertiaryText{};
    ResourceId imageId{kInvalidResourceId};
  };

  std::string trackSectionDisplayName(TrackSection const& section);
} // namespace ao::tui
