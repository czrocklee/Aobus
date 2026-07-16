// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackColumnLayoutStore.h"
#include <ao/Error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::uimodel
{
  inline constexpr std::uint32_t kTrackColumnLayoutVersion = 1;

  // Persistence DTOs: member names are the exact versioned YAML keys.
  struct StoredTrackColumn final
  {
    std::string field{};
    std::int32_t width = -1;
    double weight = -1.0;
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

  Result<TrackColumnLayoutDocument> encodeTrackColumnLayout(TrackColumnLayoutState const& state);
  Result<TrackColumnLayoutState> decodeTrackColumnLayout(TrackColumnLayoutDocument const& document);
} // namespace ao::uimodel
