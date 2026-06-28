// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace ao::uimodel
{
  std::string trackSelectionSummaryText(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration)
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
} // namespace ao::uimodel
