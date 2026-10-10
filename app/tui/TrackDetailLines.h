// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  struct TrackRow;
} // namespace ao::rt

namespace ao::tui
{
  struct DetailSectionState final
  {
    std::array<bool, 2> expanded{true, false};
    std::size_t selected = 0;
    bool revealSelected = false;
  };

  struct TrackDetailLine final
  {
    enum class Kind : std::uint8_t
    {
      Title,
      Metadata,
      Technical,
      Tags,
    };

    std::string label{};
    std::string value{};
    Kind kind = Kind::Metadata;
  };

  /// Labeled fields used to size the pane independently of the current selection.
  std::span<rt::TrackField const> trackDetailFields();

  std::vector<TrackDetailLine> trackDetailTechnicalLines(i18n::MessageCatalog const& textCatalog,
                                                         rt::TrackRow const& row);

  /// Labeled identity, facts, optional credits, and read-only tags.
  /// Missing values and an album artist identical to the artist are omitted.
  std::vector<TrackDetailLine> trackDetailLines(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row);

  /// Complete Credits subsection for the focused detail: the localized
  /// heading plus one line per aggregated credit. Empty while the aggregate is
  /// absent or empty; the TUI has no show-empty toggle. The list arrives from
  /// the runtime snapshot at the detail boundary, never from TrackRow.
  std::vector<TrackDetailLine> trackCreditDetailLines(i18n::MessageCatalog const& textCatalog,
                                                      uimodel::TrackCreditSections const& credits);
} // namespace ao::tui
