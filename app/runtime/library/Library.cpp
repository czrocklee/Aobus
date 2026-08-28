// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/Library.h>

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

#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  struct Library::Impl final
  {
    library::MusicLibrary& storage;
    LibraryChanges& changeBus;
    LibraryWriteLane writeLane;
    LibraryCommands commands;
    LibraryJobs jobs;

    Impl(async::Runtime& asyncRuntime,
         library::MusicLibrary& libraryStorage,
         library::WritableMusicLibrary writableStorage,
         LibraryChanges& changes)
      : storage{libraryStorage}
      , changeBus{changes}
      , writeLane{asyncRuntime.callbackExecutor(), std::move(writableStorage), changes}
      , commands{libraryStorage, writeLane, asyncRuntime}
      , jobs{asyncRuntime, libraryStorage, writeLane}
    {
    }
  };

  Result<std::unique_ptr<Library>> Library::create(async::Runtime& asyncRuntime,
                                                   library::MusicLibrary& storage,
                                                   LibraryChanges& changes)
  {
    auto writableStorageRes = library::WritableMusicLibrary::acquire(storage);

    if (!writableStorageRes)
    {
      return std::unexpected{writableStorageRes.error()};
    }

    auto implPtr = std::make_unique<Impl>(asyncRuntime, storage, std::move(*writableStorageRes), changes);
    return std::unique_ptr<Library>{new Library{std::move(implPtr)}};
  }

  Library::Library(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  Library::~Library()
  {
    beginClosing();
  }

  void Library::beginClosing() noexcept
  {
    _implPtr->writeLane.beginClosing();
  }

  LibrarySnapshot Library::snapshot() const
  {
    return LibrarySnapshot{_implPtr->storage};
  }

  LibraryStorageCapacity Library::storageCapacity() const
  {
    auto const capacity = _implPtr->storage.storageCapacity();
    return LibraryStorageCapacity{.mapBytes = capacity.mapBytes, .highWaterBytes = capacity.highWaterBytes};
  }

  LibraryChanges const& Library::changes() const noexcept
  {
    return _implPtr->changeBus;
  }

  LibraryCommands& Library::commands() noexcept
  {
    return _implPtr->commands;
  }

  LibraryJobs& Library::jobs() noexcept
  {
    return _implPtr->jobs;
  }

  LibraryAuthoringAvailability Library::authoringAvailability() const
  {
    return _implPtr->writeLane.availability();
  }

  async::Subscription Library::onAuthoringAvailabilityChanged(
    compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const
  {
    return _implPtr->writeLane.onAvailabilityChanged(std::move(handler));
  }

  Result<BoundTrackTargets> Library::bindTrackTargets(std::span<TrackId const> trackIds) const
  {
    return _implPtr->writeLane.bindTrackTargets(trackIds);
  }

  Result<BoundListOrder> Library::bindListOrder(ListId const listId,
                                                std::span<TrackId const> const effectiveTrackIds) const
  {
    return _implPtr->writeLane.bindListOrder(listId, effectiveTrackIds);
  }

  Result<BoundListOrder> Library::bindListOrder(ListId const listId, std::vector<TrackId>&& effectiveTrackIds) const
  {
    return _implPtr->writeLane.bindListOrder(listId, std::move(effectiveTrackIds));
  }
} // namespace ao::rt
