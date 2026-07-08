// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <vector>

namespace ao::rt
{
  struct TrackRow;
} // namespace ao::rt

namespace ao::tui
{
  struct TrackDetailLine final
  {
    std::string label{};
    std::string value{};
  };

  std::vector<TrackDetailLine> trackDetailLines(rt::TrackRow const& row);
} // namespace ao::tui
