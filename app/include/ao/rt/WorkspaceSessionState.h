// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewState.h"
#include <ao/CoreIds.h>

#include <vector>

namespace ao::rt
{
  struct WorkspaceSessionState final
  {
    std::vector<TrackListViewConfig> openViews;
    ListId activeListId = kInvalidListId;
    std::vector<CustomTrackPresentationPreset> customPresets;
  };
} // namespace ao::rt
