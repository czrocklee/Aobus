// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SelectionNavigation.h"

#include <ao/i18n/MessageCatalog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

namespace ao::tui
{
  std::string selectionSummary(i18n::MessageCatalog const& textCatalog,
                               std::size_t const trackCount,
                               std::int32_t const selectedIndex,
                               std::size_t const markedCount,
                               bool const visualSelectionActive)
  {
    auto summary = std::string{};

    if (trackCount == 0)
    {
      summary = i18n::requiredFormat(textCatalog, i18n::MessageId::TrackCount, {{"count", trackCount}});
    }
    else
    {
      auto const visibleIndex = clampSelection(static_cast<std::size_t>(std::max(0, selectedIndex)), trackCount) + 1;
      summary = std::format("{} / {}",
                            visibleIndex,
                            i18n::requiredFormat(textCatalog, i18n::MessageId::TrackCount, {{"count", trackCount}}));
    }

    if (markedCount > 0)
    {
      summary =
        std::format("{} · {}",
                    i18n::requiredFormat(textCatalog, i18n::MessageId::TuiLibraryMarkedCount, {{"count", markedCount}}),
                    summary);
    }

    if (!visualSelectionActive)
    {
      return summary;
    }

    // The mark count alone cannot say whether the next motion still grows the
    // range, so the running selection names itself ahead of the counts.
    return std::format("{} · {}", i18n::requiredText(textCatalog, i18n::MessageId::TuiLibraryVisualMode), summary);
  }

  std::int32_t moveSelection(std::int32_t const selectedIndex, std::int32_t const delta, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    auto const maxIndex =
      std::min<std::int64_t>(static_cast<std::int64_t>(itemCount - 1), std::numeric_limits<std::int32_t>::max());
    auto const next = static_cast<std::int64_t>(selectedIndex) + static_cast<std::int64_t>(delta);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(next, 0, maxIndex));
  }

  std::size_t clampSelection(std::size_t const selection, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    return std::min(selection, itemCount - 1);
  }
} // namespace ao::tui
