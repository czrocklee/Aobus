// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewState.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <vector>

namespace ao::rt
{
  struct WorkspaceSessionState final
  {
    std::vector<TrackListViewConfig> openViews{};
    std::size_t activeViewIndex = 0;
    std::vector<CustomTrackPresentationPreset> customPresets{};
  };
} // namespace ao::rt
