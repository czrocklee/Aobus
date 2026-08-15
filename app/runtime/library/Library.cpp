// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/Library.h>

#include "LibraryMutationService.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryWriter.h>

#include <expected>
#include <functional>
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
    LibraryMutationService mutationService;
    LibraryWriter writer;
    LibraryTaskService taskService;

    Impl(async::Runtime& asyncRuntime,
         library::MusicLibrary& libraryStorage,
         library::WritableMusicLibrary writableStorage,
         LibraryChanges& changes)
      : storage{libraryStorage}
      , changeBus{changes}
      , mutationService{asyncRuntime.callbackExecutor(), std::move(writableStorage), changes}
      , writer{libraryStorage, mutationService}
      , taskService{asyncRuntime, libraryStorage, mutationService}
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
    _implPtr->mutationService.beginClosing();
  }

  LibraryReader Library::reader() const
  {
    return LibraryReader{_implPtr->storage};
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

  LibraryWriter& Library::writer() noexcept
  {
    return _implPtr->writer;
  }

  LibraryTaskService& Library::taskService() noexcept
  {
    return _implPtr->taskService;
  }

  Result<ListId> Library::createList(LibraryListDraft const& draft)
  {
    return _implPtr->writer.createList(draft);
  }

  Result<UpdateListReply> Library::updateList(LibraryListDraft const& draft)
  {
    return _implPtr->writer.updateList(draft);
  }

  Result<DeleteListReply> Library::deleteList(ListId const listId, DeleteListOptions const options)
  {
    return _implPtr->writer.deleteList(listId, options);
  }

  Result<DeleteListReply> Library::previewDeleteList(ListId const listId, DeleteListOptions const options)
  {
    return _implPtr->writer.previewDeleteList(listId, options);
  }

  Result<DeleteListSubtreeReply> Library::deleteListAndDescendants(ListId const listId, DeleteListOptions const options)
  {
    return _implPtr->writer.deleteListAndDescendants(listId, options);
  }

  Result<DeleteListSubtreeReply> Library::previewDeleteListAndDescendants(ListId const listId,
                                                                          DeleteListOptions const options)
  {
    return _implPtr->writer.previewDeleteListAndDescendants(listId, options);
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

  Result<BoundListOrder> Library::bindListOrder(ListId const listId,
                                                std::span<TrackId const> const effectiveTrackIds) const
  {
    return _implPtr->mutationService.bindListOrder(listId, effectiveTrackIds);
  }

  Result<BoundListOrder> Library::bindListOrder(ListId const listId, std::vector<TrackId>&& effectiveTrackIds) const
  {
    return _implPtr->mutationService.bindListOrder(listId, std::move(effectiveTrackIds));
  }
} // namespace ao::rt
