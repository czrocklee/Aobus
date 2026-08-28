// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::uimodel
{
  inline constexpr std::uint32_t kTrackColumnLayoutVersion = 2;

  // Persistence DTOs isolate the versioned wire shape from live UIModel state.
  struct StoredTrackColumn final
  {
    std::string field{};
    std::int32_t width = -1;
    double weight = -1.0;
    bool visible = true;
  };

  struct StoredTrackColumnLayout final
  {
    std::uint32_t listId = 0;
    std::vector<StoredTrackColumn> columns{};
  };

  struct TrackColumnLayoutDocument final
  {
    std::uint32_t version = kTrackColumnLayoutVersion;
    std::vector<StoredTrackColumnLayout> layouts{};
  };

  Result<TrackColumnLayoutDocument> toTrackColumnLayoutDocument(TrackColumnLayouts::Snapshot const& state);
  Result<TrackColumnLayouts::Snapshot> trackColumnLayoutsFromDocument(TrackColumnLayoutDocument const& document);

  struct TrackColumnLayoutYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, TrackColumnLayouts::Snapshot const& state) const;
    Result<TrackColumnLayouts::Snapshot> deserialize(ryml::ConstNodeRef node,
                                                     TrackColumnLayouts::Snapshot const& seed) const;
  };
} // namespace ao::uimodel
