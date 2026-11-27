// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/Library.h>

#include "LibraryOwner.h"
#include "LibraryWriteLane.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  LibraryOwner::LibraryOwner(async::Runtime& asyncRuntime,
                             library::WritableMusicLibrary writable,
                             LibraryChanges& changes)
    : LibraryOwner{asyncRuntime, writable.library(), std::move(writable), changes}
  {
  }

  LibraryOwner::LibraryOwner(async::Runtime& asyncRuntime,
                             library::MusicLibrary& storage,
                             library::WritableMusicLibrary&& writable,
                             LibraryChanges& changes)
    : _writeLane{asyncRuntime.callbackExecutor(), std::move(writable), changes}
    , _library{storage, changes, _writeLane}
    , _commands{_library, storage, _writeLane, asyncRuntime}
    , _jobs{asyncRuntime, storage, _writeLane}
  {
  }

  LibraryOwner::~LibraryOwner()
  {
    beginClosing();
  }

  void LibraryOwner::beginClosing() noexcept
  {
    _writeLane.beginClosing();
  }

  Library::Library(library::MusicLibrary const& storage,
                   LibraryChanges const& changes,
                   LibraryWriteLane const& writeLane)
    : _storage{storage}, _changes{changes}, _writeLane{writeLane}
  {
  }

  LibrarySnapshot Library::snapshot() const
  {
    return LibrarySnapshot{_storage};
  }

  LibraryChanges const& Library::changes() const noexcept
  {
    return _changes;
  }

  LibraryAuthoringAvailability Library::authoringAvailability() const
  {
    return _writeLane.availability();
  }

  async::Subscription Library::onAuthoringAvailabilityChanged(
    compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const
  {
    return _writeLane.onAvailabilityChanged(std::move(handler));
  }

  Result<BoundTrackTargets> Library::bindTrackTargets(std::span<TrackId const> trackIds) const
  {
    return _writeLane.bindTrackTargets(trackIds);
  }

  Result<BoundTrackTargets> Library::bindTrackTargets(std::span<TrackId const> trackIds,
                                                      LibrarySnapshot const& snapshot) const
  {
    return _writeLane.bindTrackTargets(trackIds, snapshot);
  }

  Result<BoundListOrder> Library::bindListOrder(ListId const listId,
                                                std::span<TrackId const> const effectiveTrackIds) const
  {
    return _writeLane.bindListOrder(listId, effectiveTrackIds);
  }

  Result<BoundListOrder> Library::bindListOrder(ListId const listId, std::vector<TrackId>&& effectiveTrackIds) const
  {
    return _writeLane.bindListOrder(listId, std::move(effectiveTrackIds));
  }
} // namespace ao::rt
