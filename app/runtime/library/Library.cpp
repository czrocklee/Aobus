// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/Library.h>

#include "LibraryWriteLane.h"
#include <ao/Contract.h>
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
  struct Library::Prepared::Impl final
  {
    library::WritableMusicLibrary writableStorage;

    explicit Impl(library::WritableMusicLibrary storage)
      : writableStorage{std::move(storage)}
    {
    }
  };

  struct Library::Impl final
  {
    library::MusicLibrary& storage;
    LibraryChanges& changeBus;
    LibraryWriteLane writeLane;
    LibraryCommands commands;
    LibraryJobs jobs;

    Impl(async::Runtime& asyncRuntime, library::WritableMusicLibrary writableStorage, LibraryChanges& changes)
      : storage{writableStorage.library()}
      , changeBus{changes}
      , writeLane{asyncRuntime.callbackExecutor(), std::move(writableStorage), changes}
      , commands{storage, writeLane, asyncRuntime}
      , jobs{asyncRuntime, storage, writeLane}
    {
    }
  };

  Result<Library::Prepared> Library::prepare(library::MusicLibrary& storage)
  {
    auto writableStorageRes = library::WritableMusicLibrary::acquire(storage);

    if (!writableStorageRes)
    {
      return std::unexpected{writableStorageRes.error()};
    }

    return Prepared{std::make_unique<Prepared::Impl>(std::move(*writableStorageRes))};
  }

  Library::Prepared::Prepared(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  Library::Prepared::~Prepared() = default;
  Library::Prepared::Prepared(Prepared&&) noexcept = default;
  Library::Prepared& Library::Prepared::operator=(Prepared&&) noexcept = default;

  Library::Library(async::Runtime& asyncRuntime, Prepared prepared, LibraryChanges& changes)
  {
    AO_EXPECTS(prepared._implPtr != nullptr);
    _implPtr = std::make_unique<Impl>(asyncRuntime, std::move(prepared._implPtr->writableStorage), changes);
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
