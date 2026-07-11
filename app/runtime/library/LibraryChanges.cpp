// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryChanges.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ao::rt
{
  struct LibraryChanges::Impl final
  {
    Signal<std::vector<TrackId> const&> tracksMutatedSignal;
    Signal<LibraryChanges::TrackCollectionChanged const&> trackCollectionChangedSignal;
    Signal<LibraryChanges::ListsMutated const&> listsMutatedSignal;
    Signal<std::size_t> libraryTaskCompletedSignal;
    Signal<LibraryChanges::LibraryTaskProgressUpdated const&> libraryTaskProgressSignal;
  };

  LibraryChanges::LibraryChanges()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  LibraryChanges::~LibraryChanges() = default;

  Subscription LibraryChanges::onTracksMutated(std::move_only_function<void(std::vector<TrackId> const&)> handler) const
  {
    return _implPtr->tracksMutatedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onTrackCollectionChanged(
    std::move_only_function<void(TrackCollectionChanged const&)> handler) const
  {
    return _implPtr->trackCollectionChangedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onListsMutated(std::move_only_function<void(ListsMutated const&)> handler) const
  {
    return _implPtr->listsMutatedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler) const
  {
    return _implPtr->libraryTaskCompletedSignal.connect(std::move(handler));
  }

  Subscription LibraryChanges::onLibraryTaskProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const
  {
    return _implPtr->libraryTaskProgressSignal.connect(std::move(handler));
  }

  void LibraryChanges::notifyTracksMutated(std::vector<TrackId> trackIds)
  {
    _implPtr->tracksMutatedSignal.emit(trackIds);
  }

  void LibraryChanges::notifyTrackCollectionChanged(std::vector<TrackId> inserted, std::vector<TrackId> deleted)
  {
    _implPtr->trackCollectionChangedSignal.emit({.inserted = std::move(inserted), .deleted = std::move(deleted)});
  }

  void LibraryChanges::notifyListsMutated(std::vector<ListId> upserted,
                                          std::vector<ListId> deleted,
                                          std::vector<ManualListContentChange> manualContentChanges)
  {
    _implPtr->listsMutatedSignal.emit({.upserted = std::move(upserted),
                                       .deleted = std::move(deleted),
                                       .manualContentChanges = std::move(manualContentChanges)});
  }

  void LibraryChanges::notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress)
  {
    _implPtr->libraryTaskProgressSignal.emit(progress);
  }

  void LibraryChanges::notifyLibraryTaskCompleted(std::size_t count)
  {
    _implPtr->libraryTaskCompletedSignal.emit(count);
  }
} // namespace ao::rt
