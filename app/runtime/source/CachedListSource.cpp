// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/CachedListSource.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

namespace ao::rt
{
  CachedListSource::CachedListSource(CachedListSourceDefinition definition,
                                     std::unique_ptr<TrackSource> implementationPtr)
    : _definition{std::move(definition)}, _implementationPtr{std::move(implementationPtr)}
  {
    if (_implementationPtr == nullptr)
    {
      throwException<Exception>("Cached list source requires an implementation");
    }

    _lastPublishedSize = _implementationPtr->size();
    subscribeToImplementation();
  }

  CachedListSource::~CachedListSource()
  {
    _implementationSubscription.reset();
    _implementationPtr.reset();
  }

  void CachedListSource::rebind(CachedListSourceDefinition definition, std::unique_ptr<TrackSource> implementationPtr)
  {
    if (implementationPtr == nullptr)
    {
      throwException<Exception>("Cached list source rebind requires an implementation");
    }

    if (state() == TrackSourceState::Invalidated)
    {
      throwException<Exception>("Cannot rebind an invalidated cached list source");
    }

    auto const previousSize = _lastPublishedSize;
    _implementationSubscription.reset();
    _implementationPtr.reset();
    _definition = std::move(definition);
    _implementationPtr = std::move(implementationPtr);
    _lastPublishedSize = _implementationPtr->size();
    subscribeToImplementation();

    std::ignore = publishDelta(SourceReset{}, previousSize);
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

  void CachedListSource::applyManualEditScript(delta::RegularTrackEditScript const& script)
  {
    auto& source = manualImplementation();
    source.applyManualEditScript(script);
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

  void CachedListSource::discardSnapshot() noexcept
  {
    _implementationSubscription.reset();
    _implementationPtr->invalidate();
    _lastPublishedSize = 0;
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
    _implementationSubscription = _implementationPtr->subscribe([this](TrackSourceDelta const& batch) noexcept
                                                                { handleImplementationBatch(batch); });
  }

  void CachedListSource::handleImplementationBatch(TrackSourceDelta const& batch)
  {
    if (std::holds_alternative<SourceInvalidated>(batch))
    {
      semanticInvalidate();
      return;
    }

    auto forwarded = batch;
    auto const previousSize = _lastPublishedSize;
    _lastPublishedSize = _implementationPtr->size();

    std::ignore = publishDelta(std::move(forwarded), previousSize);
  }
} // namespace ao::rt
