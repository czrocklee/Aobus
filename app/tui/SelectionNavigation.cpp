// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SelectionNavigation.h"

#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>

namespace ao::tui
{
  std::int32_t navigationPageRows(ftxui::Box const& viewport)
  {
    constexpr std::int32_t kUnmeasuredPageRows = 10;
    return viewport.IsEmpty() ? kUnmeasuredPageRows : std::max(1, viewport.y_max - viewport.y_min + 1);
  }

  std::optional<std::int32_t> listNavigationDelta(ftxui::Event const& event,
                                                  std::int32_t const pageRows,
                                                  bool const vimKeys)
  {
    if (event == ftxui::Event::ArrowUp || (vimKeys && event == ftxui::Event::Character("k")))
    {
      return -1;
    }

    if (event == ftxui::Event::ArrowDown || (vimKeys && event == ftxui::Event::Character("j")))
    {
      return 1;
    }

    if (event == ftxui::Event::PageUp)
    {
      return -std::max(1, pageRows);
    }

    if (event == ftxui::Event::PageDown)
    {
      return std::max(1, pageRows);
    }

    if (event == ftxui::Event::Home)
    {
      return -std::numeric_limits<std::int32_t>::max();
    }

    if (event == ftxui::Event::End)
    {
      return std::numeric_limits<std::int32_t>::max();
    }

    return std::nullopt;
  }

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
