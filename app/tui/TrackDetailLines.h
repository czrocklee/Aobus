// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>

#include <span>
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

  /**
   * @brief Every field Detail can show, in display order.
   *
   * A pane sized from the tracks it happens to show would resize while the
   * selection moves, so geometry is measured against this complete set rather
   * than against the rows one track produces.
   */
  std::span<rt::TrackField const> trackDetailFields();

  /**
   * @brief The rows Detail shows for @p row.
   *
   * Core identity fields are always present, keeping a placeholder when the
   * track genuinely lacks them. Every other field appears only when it carries
   * a value, so a sparse track reads as short rather than as a column of
   * dashes.
   */
  std::vector<TrackDetailLine> trackDetailLines(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row);
} // namespace ao::tui
