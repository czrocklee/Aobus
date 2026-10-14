// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewState.h"
#include <ao/utility/Uuid.h>

#include <cstddef>
#include <vector>

namespace ao::rt
{
  struct WorkspaceSessionState final
  {
    // The library the session was saved for; its list ids mean nothing in another.
    utility::UuidBytes libraryId{};
    std::vector<TrackListViewConfig> openViews{};
    std::size_t activeViewIndex = 0;
    std::vector<CustomTrackPresentationPreset> customPresets{};
  };
} // namespace ao::rt
