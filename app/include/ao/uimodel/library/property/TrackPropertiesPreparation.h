// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/LibraryEditing.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <span>

namespace ao::rt
{
  class LibrarySnapshot;
}

namespace ao::uimodel
{
  class TrackPropertiesFormModel;
  struct TrackPropertiesFormSpec;

  /// Prepare the owning form and binding from one snapshot; on failure the form is unchanged.
  /// Read any additional selection facts before releasing the snapshot. Subscribe to invalidation
  /// and check isCurrent() before installing the draft.
  Result<TrackAuthoringSession> prepareTrackProperties(LibraryEditing editing,
                                                       rt::LibrarySnapshot const& snapshot,
                                                       std::span<TrackId const> targetIds,
                                                       TrackPropertiesFormSpec const& spec,
                                                       TrackPropertiesFormModel& form);
} // namespace ao::uimodel
