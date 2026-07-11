// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include <ao/CoreIds.h>

#include <string>
#include <vector>

namespace ao::rt
{
  struct TrackOrderSpec final
  {
    std::vector<TrackSortTerm> sortBy{};

    bool operator==(TrackOrderSpec const&) const = default;
  };

  struct PlaybackLaunchContext final
  {
    ListId sourceListId = kInvalidListId;
    std::string quickFilterExpression{};
    TrackOrderSpec order{};

    bool operator==(PlaybackLaunchContext const&) const = default;
  };
} // namespace ao::rt
