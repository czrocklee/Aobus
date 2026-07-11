// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/CachedListSource.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

namespace ao::rt
{
  CachedListSource::CachedListSource(ListId const listId,
                                     CachedListSourceDefinition definition,
                                     TrackSourceLease parentLease,
                                     std::unique_ptr<TrackSource> implementationPtr)
    : _listId{listId}
    , _definition{std::move(definition)}
    , _parentLease{std::move(parentLease)}
    , _implementationPtr{std::move(implementationPtr)}
  {
    if (_implementationPtr == nullptr)
    {
      throwException<Exception>("Cached list source requires an implementation");
    }

    _publishedSize = _implementationPtr->size();
    subscribeToImplementation();
  }

  CachedListSource::~CachedListSource()
  {
    _implementationSubscription.reset();
    _implementationPtr.reset();
  }

  void CachedListSource::rebind(CachedListSourceDefinition definition,
                                TrackSourceLease parentLease,
                                std::unique_ptr<TrackSource> implementationPtr)
  {
    if (implementationPtr == nullptr)
    {
      throwException<Exception>("Cached list source rebind requires an implementation");
    }

    if (state() == TrackSourceState::Invalidated)
    {
      throwException<Exception>("Cannot rebind an invalidated cached list source");
    }

    auto const previousSize = _publishedSize;
    _implementationSubscription.reset();
    _implementationPtr.reset();
    _definition = std::move(definition);
    _parentLease = std::move(parentLease);
    _implementationPtr = std::move(implementationPtr);
    _publishedSize = _implementationPtr->size();
    subscribeToImplementation();

    std::ignore = publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceReset{}}}, previousSize);
  }

  bool CachedListSource::trySynchronizeManualDefinition(CachedListSourceDefinition const& definition)
  {
    if (_definition.kind != CachedListSourceKind::Manual || definition.kind != CachedListSourceKind::Manual)
    {
      return false;
    }

    auto* const source = dynamic_cast<ManualListSource*>(_implementationPtr.get());

    if (source == nullptr)
    {
      return false;
    }

    auto actualDefinition = _definition;
    auto const storedTrackIds = source->storedTrackIds();
    actualDefinition.storedTrackIds.assign(storedTrackIds.begin(), storedTrackIds.end());

    if (actualDefinition != definition)
    {
      return false;
    }

    _definition = definition;
    return true;
  }

  void CachedListSource::semanticInvalidate()
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

    _implementationSubscription.reset();
    invalidate();
  }

  void CachedListSource::applyManualTracksInsert(ManualTracksInsert const& operation)
  {
    auto& source = manualImplementation();

    try
    {
      source.applyManualTracksInsert(operation);
    }
    catch (...)
    {
      syncManualDefinition(source);
      throw;
    }

    syncManualDefinition(source);
  }

  void CachedListSource::applyManualTracksRemove(ManualTracksRemove const& operation)
  {
    auto& source = manualImplementation();

    try
    {
      source.applyManualTracksRemove(operation);
    }
    catch (...)
    {
      syncManualDefinition(source);
      throw;
    }

    syncManualDefinition(source);
  }

  void CachedListSource::applyManualTracksMove(ManualTracksMove const& operation)
  {
    auto& source = manualImplementation();

    try
    {
      source.applyManualTracksMove(operation);
    }
    catch (...)
    {
      syncManualDefinition(source);
      throw;
    }

    syncManualDefinition(source);
  }

  std::size_t CachedListSource::size() const
  {
    return _implementationPtr->size();
  }

  TrackId CachedListSource::trackIdAt(std::size_t const index) const
  {
    return _implementationPtr->trackIdAt(index);
  }

  std::optional<std::size_t> CachedListSource::indexOf(TrackId const id) const
  {
    return _implementationPtr->indexOf(id);
  }

  ManualListSource& CachedListSource::manualImplementation()
  {
    if (_definition.kind != CachedListSourceKind::Manual)
    {
      throwException<Exception>("Detailed manual operation targeted a non-manual cached source");
    }

    auto* const source = dynamic_cast<ManualListSource*>(_implementationPtr.get());

    if (source == nullptr)
    {
      throwException<Exception>("Cached manual source has an incompatible implementation");
    }

    return *source;
  }

  void CachedListSource::syncManualDefinition(ManualListSource const& source)
  {
    auto const storedTrackIds = source.storedTrackIds();
    _definition.storedTrackIds.assign(storedTrackIds.begin(), storedTrackIds.end());
  }

  void CachedListSource::subscribeToImplementation()
  {
    _implementationSubscription =
      _implementationPtr->subscribe([this](TrackSourceDeltaBatch const& batch) { handleImplementationBatch(batch); });
  }

  void CachedListSource::handleImplementationBatch(TrackSourceDeltaBatch const& batch)
  {
    if (batch.deltas.size() == 1 && std::holds_alternative<SourceInvalidated>(batch.deltas.front()))
    {
      semanticInvalidate();
      return;
    }

    auto forwarded = batch;
    forwarded.revision = 0;
    auto const previousSize = _publishedSize;
    _publishedSize = _implementationPtr->size();

    std::ignore = publishDeltaBatch(std::move(forwarded), previousSize);
  }
} // namespace ao::rt
