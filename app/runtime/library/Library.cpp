// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryMutationService.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryWriter.h>

#include <functional>
#include <memory>
#include <span>
#include <utility>

namespace ao::rt
{
  namespace
  {
    library::WritableMusicLibrary acquireWritableLibrary(library::MusicLibrary& storage)
    {
      auto result = library::WritableMusicLibrary::acquire(storage);

      if (!result)
      {
        throwException<Exception>("Failed to acquire writable library session: {}", result.error().message);
      }

      return std::move(*result);
    }
  } // namespace

  struct Library::Impl final
  {
    library::MusicLibrary& storage;
    LibraryChanges& changeBus;
    LibraryMutationService mutationService;
    LibraryWriter writer;
    LibraryTaskService taskService;

    Impl(async::Runtime& asyncRuntime, library::MusicLibrary& libraryStorage, LibraryChanges& changes)
      : storage{libraryStorage}
      , changeBus{changes}
      , mutationService{asyncRuntime.callbackExecutor(), acquireWritableLibrary(libraryStorage), changes}
      , writer{libraryStorage, mutationService}
      , taskService{asyncRuntime, libraryStorage, changes, mutationService}
    {
    }
  };

  Library::Library(async::Runtime& asyncRuntime, library::MusicLibrary& storage, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, storage, changes)}
  {
  }

  Library::~Library() = default;

  LibraryReader Library::reader() const
  {
    return LibraryReader{_implPtr->storage};
  }

  LibraryChanges const& Library::changes() const noexcept
  {
    return _implPtr->changeBus;
  }

  LibraryWriter& Library::writer() noexcept
  {
    return _implPtr->writer;
  }

  LibraryTaskService& Library::taskService() noexcept
  {
    return _implPtr->taskService;
  }

  LibraryAuthoringAvailability Library::authoringAvailability() const
  {
    return _implPtr->mutationService.availability();
  }

  async::Subscription Library::onAuthoringAvailabilityChanged(
    std::move_only_function<void(LibraryAuthoringAvailability const&)> handler) const
  {
    return _implPtr->mutationService.onAvailabilityChanged(std::move(handler));
  }

  Result<BoundTrackTargets> Library::bindTrackTargets(std::span<TrackId const> trackIds) const
  {
    return _implPtr->mutationService.bindTrackTargets(trackIds);
  }
} // namespace ao::rt
