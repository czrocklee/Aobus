// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>

#include <string>
#include <string_view>

namespace ao::tui
{
  /// The one visible marker for a missing track field in the table and the
  /// plain track label. A value that legitimately reads the same stays data.
  inline constexpr std::string_view kMissingTrackFieldPlaceholder = "-";

  struct TrackListEntry final
  {
    TrackId id{};
    ResourceId coverArtId{kInvalidResourceId};
    rt::TrackRow row{};
    std::string label{};
    std::string detail{};
  };

  std::string trackDisplayTitle(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row);
  std::string trackDisplayDetail(rt::TrackRow const& row);
  TrackListEntry makeTrackListEntry(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row);
  std::string trackTableLabel(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row);
} // namespace ao::tui
