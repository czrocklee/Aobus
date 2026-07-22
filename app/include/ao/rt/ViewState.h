// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackField.h"
#include "TrackPresentation.h"
#include "ViewIds.h"
#include "VirtualListIds.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct TrackListViewState final
  {
    ViewId id{};
    ListId listId{};
    std::string filterExpression{};
    std::optional<Error> optFilterError = std::nullopt;
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackId> selection{};
    TrackPresentationSpec presentation{};
  };

  struct TrackListViewConfig final
  {
    ListId listId = kAllTracksListId;
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::optional<TrackPresentationSpec> optPresentation{};
  };

  enum class GlobalViewKind : std::uint8_t
  {
    AllTracks,
  };
} // namespace ao::rt
