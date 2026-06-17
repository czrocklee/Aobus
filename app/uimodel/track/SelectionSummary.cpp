// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/track/SelectionSummary.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace ao::uimodel::track
{
  std::string selectionSummaryText(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration)
  {
    if (count == 0)
    {
      return {};
    }

    auto text = std::format("{} {}", count, count == 1 ? "item selected" : "items selected");

    if (optTotalDuration && *optTotalDuration > std::chrono::milliseconds{0})
    {
      text += std::format(" ({})", formatDuration(*optTotalDuration));
    }

    return text;
  }
} // namespace ao::uimodel::track
