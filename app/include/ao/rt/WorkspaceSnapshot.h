// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewIds.h"

#include <cstdint>
#include <vector>

namespace ao::rt
{
  struct NavigationAvailability final
  {
    bool canGoBack = false;
    bool canGoForward = false;

    bool operator==(NavigationAvailability const&) const = default;
  };

  struct WorkspaceSnapshot final
  {
    ViewId activeViewId = kInvalidViewId;
    std::vector<ViewId> openViews{};
    std::vector<CustomTrackPresentationPreset> customPresets{};
    NavigationAvailability navigation{};
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

  enum class WorkspaceCommitDisposition : std::uint8_t
  {
    Applied,
    NoChange,
  };

  struct WorkspaceCommitReceipt final
  {
    WorkspaceCommitDisposition disposition = WorkspaceCommitDisposition::NoChange;
    std::uint64_t beforeRevision = 0;
    std::uint64_t afterRevision = 0;
    ViewId activeViewId = kInvalidViewId;

    bool operator==(WorkspaceCommitReceipt const&) const = default;
  };
} // namespace ao::rt
