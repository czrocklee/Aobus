// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <span>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;
  class LibrarySnapshot;
  class LibraryWriteLane;

  /// Read and observe one live library. Bindings identify targets; they do not authorize submission.
  class Library final
  {
  public:
    Library(Library const&) = delete;
    Library& operator=(Library const&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    LibrarySnapshot snapshot() const;
    LibraryChanges const& changes() const noexcept;
    LibraryAuthoringAvailability authoringAvailability() const;
    // Synchronous callback-executor notification. Defer owner shutdown to a later executor turn.
    async::Subscription onAuthoringAvailabilityChanged(
      compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    /// Validates targets at this storage's snapshot revision, which must still be available.
    /// Later commits can invalidate the returned binding before submission.
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds,
                                               LibrarySnapshot const& snapshot) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::span<TrackId const> effectiveTrackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::vector<TrackId>&& effectiveTrackIds) const;

  private:
    friend class LibraryOwner;
    Library(library::MusicLibrary const& storage, LibraryChanges const& changes, LibraryWriteLane const& writeLane);
    ~Library() = default;

    library::MusicLibrary const& _storage;
    LibraryChanges const& _changes;
    LibraryWriteLane const& _writeLane;
  };
} // namespace ao::rt
