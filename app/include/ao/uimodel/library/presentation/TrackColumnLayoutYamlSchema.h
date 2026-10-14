// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/utility/Uuid.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  inline constexpr auto kTrackColumnLayoutsConfigGroup = std::string_view{"trackView.columnLayouts"};
  inline constexpr std::uint32_t kTrackColumnLayoutVersion = 3;

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
    utility::UuidBytes libraryId{};
    std::vector<StoredTrackColumnLayout> layouts{};
  };

  Result<TrackColumnLayoutDocument> toTrackColumnLayoutDocument(TrackColumnLayouts::Snapshot const& state,
                                                                utility::UuidBytes const& libraryId);
  Result<TrackColumnLayouts::Snapshot> trackColumnLayoutsFromDocument(TrackColumnLayoutDocument const& document);

  // Bound to the library whose list ids the layouts name. A document saved for
  // another library keeps only its virtual-list layouts.
  class TrackColumnLayoutYamlSchema final
  {
  public:
    explicit TrackColumnLayoutYamlSchema(utility::UuidBytes const& libraryId)
      : _libraryId{libraryId}
    {
    }

    Result<> serialize(ryml::NodeRef node, TrackColumnLayouts::Snapshot const& state) const;
    Result<TrackColumnLayouts::Snapshot> deserialize(ryml::ConstNodeRef node,
                                                     TrackColumnLayouts::Snapshot const& seed) const;

  private:
    utility::UuidBytes _libraryId;
  };
} // namespace ao::uimodel
