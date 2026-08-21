// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
  class PresentationTextCatalog;
}

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

  std::string trackDisplayTitle(uimodel::PresentationTextCatalog const& textCatalog, rt::TrackRow const& row);
  std::string trackDisplayDetail(rt::TrackRow const& row);
  TrackListEntry makeTrackListEntry(uimodel::PresentationTextCatalog const& textCatalog, rt::TrackRow const& row);
  std::string trackTableLabel(uimodel::PresentationTextCatalog const& textCatalog, rt::TrackRow const& row);
  std::vector<std::string> menuLabels(std::vector<TrackListEntry> const& tracks);
} // namespace ao::tui
