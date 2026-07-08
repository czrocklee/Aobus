// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>

#include <string>
#include <vector>

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

  std::string trackDisplayTitle(rt::TrackRow const& row);
  std::string trackDisplayDetail(rt::TrackRow const& row);
  TrackListEntry makeTrackListEntry(rt::TrackRow const& row);
  std::string trackTableLabel(rt::TrackRow const& row);
  std::vector<std::string> menuLabels(std::vector<TrackListEntry> const& tracks);
} // namespace ao::tui
