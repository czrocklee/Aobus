// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::tui
{
  inline constexpr std::int32_t kMinimumTrackColumnWidthColumns = 8;
  inline constexpr std::int32_t kMaximumTrackColumnWidthColumns = 160;
  inline constexpr std::int32_t kTrackTablePlayingColumns = 2;
  inline constexpr std::string_view kTrackTableLeadingColumnSeparator = "  ";
  inline constexpr std::string_view kTrackTableInterColumnSeparator = "| ";
  inline constexpr std::string_view kTrackTableTrailingColumnSeparator = "|";
  inline constexpr std::int32_t kTrackTableScrollIndicatorColumns = 1;

  constexpr std::int32_t trackTableChromeColumns(std::size_t const columnCount) noexcept
  {
    auto const leadingColumns = columnCount == 0 ? 0 : kTrackTableLeadingColumnSeparator.size();
    auto const interColumnSeparators = columnCount == 0 ? 0 : columnCount - 1;
    auto const interColumnSeparatorColumns = interColumnSeparators * kTrackTableInterColumnSeparator.size();
    auto const trailingColumns = columnCount == 0 ? 0 : kTrackTableTrailingColumnSeparator.size();

    return kTrackTablePlayingColumns +
           static_cast<std::int32_t>(leadingColumns + interColumnSeparatorColumns + trailingColumns) +
           kTrackTableScrollIndicatorColumns;
  }

  struct TerminalTrackColumn final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t columns = kMinimumTrackColumnWidthColumns;

    bool operator==(TerminalTrackColumn const&) const = default;
  };

  struct TerminalTrackColumnLayout final
  {
    std::vector<TerminalTrackColumn> columns{};
    std::int32_t availableColumns = 0;

    bool operator==(TerminalTrackColumnLayout const&) const = default;
  };

  TerminalTrackColumnLayout projectTerminalTrackColumnLayout(rt::TrackPresentationSpec const& presentation,
                                                             std::span<uimodel::TrackColumnState const> storedLayout,
                                                             std::int32_t availableColumns);

  std::vector<uimodel::TrackColumnState> resizeTerminalTrackColumnLayout(
    rt::TrackPresentationSpec const& presentation,
    std::span<uimodel::TrackColumnState const> storedLayout,
    rt::TrackField resizedField,
    std::int32_t targetColumns,
    std::int32_t availableColumns);
} // namespace ao::tui
