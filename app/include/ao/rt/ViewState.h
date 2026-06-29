// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "TrackField.h"
#include "TrackPresentation.h"
#include <ao/CoreIds.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  enum class ViewLifecycleState : std::uint8_t
  {
    Attached,
    Detached,
    Destroyed,
  };

  enum class ViewKind : std::uint8_t
  {
    TrackList,
  };

  struct TrackListViewState final
  {
    ViewId id{};
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
    ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackId> selection{};
    TrackPresentationSpec presentation{};
    std::uint64_t revision = 0;
  };

  struct TrackListViewConfig final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::optional<TrackPresentationSpec> optPresentation{};
  };

  struct ViewRecord final
  {
    ViewId id{};
    ViewKind kind = ViewKind::TrackList;
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
  };

  enum class GlobalViewKind : std::uint8_t
  {
    AllTracks,
  };
} // namespace ao::rt
