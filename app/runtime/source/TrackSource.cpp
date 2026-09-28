// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/source/TrackSource.h>

#include <ao/Contract.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <cstddef>
#include <tuple>
#include <utility>
#include <variant>

namespace ao::rt
{
  TrackSource::~TrackSource() = default;

  async::Subscription TrackSource::subscribe(compat::MoveOnlyFunction<void(TrackSourceDelta const&)> handler) const
  {
    AO_EXPECTS(static_cast<bool>(handler), "Track source subscription handler must not be empty");

    if (_state == TrackSourceState::Invalidated)
    {
      handler(SourceInvalidated{});
      return {};
    }

    return _changedSignal.connect(std::move(handler));
  }

  void TrackSource::invalidate() noexcept
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return;
    }

    _state = TrackSourceState::Invalidated;
    discardSnapshot();
    _changedSignal.emit(SourceInvalidated{});
    _changedSignal.disconnectAll();
  }

  void TrackSource::notifyReset()
  {
    std::ignore = tryPublishDelta(SourceReset{}, size());
  }

  bool TrackSource::tryPublishDelta(TrackSourceDelta message, std::size_t const previousSize)
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return false;
    }

    AO_INVARIANT(isValidTrackSourceDelta(message, previousSize) && !std::holds_alternative<SourceInvalidated>(message));

    _changedSignal.emit(message);
    return true;
  }
} // namespace ao::rt
