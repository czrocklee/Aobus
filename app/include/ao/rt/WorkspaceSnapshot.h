// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewIds.h"

#include <cstdint>
#include <vector>

namespace ao::rt
{
  struct WorkspaceSnapshot final
  {
    ViewId activeViewId = kInvalidViewId;
    std::vector<ViewId> openViews{};
    std::vector<CustomTrackPresentationPreset> customPresets{};
    std::uint64_t revision = 0;

    bool operator==(WorkspaceSnapshot const&) const = default;
  };

  enum class WorkspaceChangeCause : std::uint8_t
  {
    Navigation,
    Focus,
    Close,
    Presentation,
    Presets,
    Restore,
    ListDeletion,
  };

  struct WorkspaceChanged final
  {
    WorkspaceSnapshot snapshot{};
    WorkspaceChangeCause cause = WorkspaceChangeCause::Navigation;

    bool operator==(WorkspaceChanged const&) const = default;
  };
} // namespace ao::rt
