// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <cstddef>
#include <format>
#include <string>

namespace ao::uimodel
{
  std::string formatTrackCount(std::size_t const count)
  {
    return std::format("{} {}", count, count == 1 ? "track" : "tracks");
  }
} // namespace ao::uimodel
