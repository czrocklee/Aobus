// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>

#include <string>

namespace ao::tui
{
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
