// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>

namespace ao::uimodel
{
  bool shouldRefreshTrackSelectionSummary(rt::LibraryChangeSet const& changes, std::span<TrackId const> const selection)
  {
    if (selection.empty())
    {
      return false;
    }

    if (changes.libraryReset)
    {
      return true;
    }

    auto const changedCount =
      changes.tracksInserted.size() + changes.tracksMutated.size() + changes.tracksDeleted.size();

    if (changedCount == 0)
    {
      return false;
    }

    auto const anyChanged = [&changes](auto contains)
    {
      return std::ranges::any_of(changes.tracksMutated, contains) ||
             std::ranges::any_of(changes.tracksDeleted, contains) ||
             std::ranges::any_of(changes.tracksInserted, contains);
    };

    if (selection.size() == 1 || changedCount == 1)
    {
      return anyChanged([selection](TrackId id) { return std::ranges::contains(selection, id); });
    }

    // Bulk rescans and multi-track selections need not have the same ordering.
    auto const selected = std::unordered_set<TrackId>{selection.begin(), selection.end()};
    return anyChanged([&selected](TrackId id) { return selected.contains(id); });
  }

  std::string trackSelectionSummaryText(i18n::MessageCatalog const& textCatalog,
                                        std::size_t const count,
                                        std::optional<std::chrono::milliseconds> const optTotalDuration)
  {
    auto duration = std::string{};

    if (optTotalDuration && *optTotalDuration > std::chrono::milliseconds{0})
    {
      duration = formatDuration(*optTotalDuration);
    }

    return trackSelectionSummary(textCatalog, count, duration);
  }
} // namespace ao::uimodel
