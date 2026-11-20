// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  struct LibraryChangeSet;
}

namespace ao::uimodel
{
  /// Whether this publication can change the duration of the stored selection.
  bool shouldRefreshTrackSelectionSummary(rt::LibraryChangeSet const& changes, std::span<TrackId const> selection);

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
  std::string trackSelectionSummaryText(i18n::MessageCatalog const& textCatalog,
                                        std::size_t count,
                                        std::optional<std::chrono::milliseconds> optTotalDuration = std::nullopt);
} // namespace ao::uimodel
