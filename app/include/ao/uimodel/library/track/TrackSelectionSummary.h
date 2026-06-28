// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ao::uimodel
{
  /**
   * Render the selection status text shown in the status bar.
   *
   * Contract:
   *   count == 0                      -> ""              (nothing selected)
   *   count == 1                      -> "1 item selected"
   *   count >  1                      -> "N items selected"
   * When a positive total duration is supplied, " (<h:mm:ss>)" is appended,
   * formatted via formatDuration. A null or non-positive duration adds nothing.
   */
  std::string trackSelectionSummaryText(std::size_t count,
                                        std::optional<std::chrono::milliseconds> optTotalDuration = std::nullopt);
} // namespace ao::uimodel
