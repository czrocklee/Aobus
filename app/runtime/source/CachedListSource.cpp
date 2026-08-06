// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/CachedListSource.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/ListOrderSource.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

namespace ao::rt
{
  CachedListSource::CachedListSource(CachedListSourceDefinition definition,
                                     std::unique_ptr<ListOrderSource> implementationPtr)
    : _definition{std::move(definition)}
    , _implementationPtr{std::move(implementationPtr)}
    , _lastPublishedSize{_implementationPtr ? _implementationPtr->size() : 0}
  {
    gsl_Expects(_implementationPtr != nullptr && "Cached list source requires an implementation");

    subscribeToImplementation();
  }

  CachedListSource::~CachedListSource()
  {
    _implementationSubscription.reset();
    _implementationPtr.reset();
  }

  void CachedListSource::rebind(CachedListSourceDefinition definition,
                                std::unique_ptr<ListOrderSource> implementationPtr)
  {
    gsl_Expects(implementationPtr != nullptr && "Cached list source rebind requires an implementation");
    gsl_Expects(state() != TrackSourceState::Invalidated && "Cannot rebind an invalidated cached list source");

    auto const previousSize = _lastPublishedSize;
    _implementationSubscription.reset();
    _implementationPtr.reset();
    _definition = std::move(definition);
    _implementationPtr = std::move(implementationPtr);
    _lastPublishedSize = _implementationPtr->size();
    subscribeToImplementation();

    std::ignore = publishDelta(SourceReset{}, previousSize);
  }

  std::optional<Error> CachedListSource::filterError() const
  {
    auto const* const filterSource = dynamic_cast<SmartListSource const*>(&_implementationPtr->filteredParent());
    return filterSource == nullptr ? std::nullopt : filterSource->error();
  }

  bool CachedListSource::trySynchronizeOrderDefinition(CachedListSourceDefinition const& definition)
  {
    auto actualDefinition = _definition;
    auto const orderTrackIds = _implementationPtr->orderTrackIds();
    actualDefinition.orderTrackIds.assign(orderTrackIds.begin(), orderTrackIds.end());

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

  void CachedListSource::applyOrderEditScript(delta::RegularTrackEditScript const& script)
  {
    _implementationPtr->applyOrderEditScript(script);
    syncOrderDefinition(*_implementationPtr);
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

  void CachedListSource::syncOrderDefinition(ListOrderSource const& source)
  {
    auto const orderTrackIds = source.orderTrackIds();
    _definition.orderTrackIds.assign(orderTrackIds.begin(), orderTrackIds.end());
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
