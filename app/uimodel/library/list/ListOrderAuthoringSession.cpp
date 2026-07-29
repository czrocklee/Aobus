// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/library/list/ListOrderAuthoringSession.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  struct ListOrderAuthoringSession::Impl final
  {
    Impl(rt::Library& libraryValue,
         rt::ViewService& viewsValue,
         rt::ViewId const viewIdValue,
         rt::BoundListOrder orderValue,
         ListOrderCapabilityState capabilitiesValue,
         std::shared_ptr<rt::TrackListProjection> projectionValuePtr)
      : library{libraryValue}
      , views{viewsValue}
      , viewId{viewIdValue}
      , order{std::move(orderValue)}
      , capabilities{std::move(capabilitiesValue)}
      , projectionPtr{std::move(projectionValuePtr)}
    {
      availabilitySubscription = library.onAuthoringAvailabilityChanged(
        [this](rt::LibraryAuthoringAvailability const& availability) noexcept
        {
          if (availability.state == rt::LibraryAuthoringState::Maintenance)
          {
            capabilities.disabledReason =
              "Library is busy. Manual ordering will be available when maintenance finishes.";
          }

          if (availability.state != rt::LibraryAuthoringState::Available ||
              availability.runtimeInstanceId != order.runtimeInstanceId() ||
              availability.libraryRevision != order.libraryRevision())
          {
            invalidate();
          }
        });
      presentationSubscription = views.onPresentationChanged(
        [this](rt::ViewService::PresentationChanged const& event) noexcept
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      projectionReplacementSubscription = views.onProjectionChanged(
        [this](rt::TrackListProjectionChanged const& event) noexcept
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      viewDestroyedSubscription = views.onViewDestroyed(
        [this](rt::ViewService::ViewDestroyed const& event) noexcept
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      projectionSubscription = projectionPtr->subscribe(
        [this](rt::TrackListProjectionDeltaBatch const&) noexcept
        {
          if (projectionEventsArmed)
          {
            invalidate();
          }
        });
      armProjectionEvents();
    }

    void armProjectionEvents() noexcept { projectionEventsArmed = true; }

    void invalidate()
    {
      if (!current || submitting)
      {
        return;
      }

      current = false;
      capabilities.canAuthorOrder = false;
      capabilities.canGapMove = false;
      capabilities.canRelativeMove = false;
      capabilities.canAbsoluteMove = false;
      capabilities.canResetOrder = false;
      capabilities.canForgetHiddenPositions = false;

      if (capabilities.disabledReason.empty())
      {
        capabilities.disabledReason = "The List changed. Start the order action again.";
      }

      invalidated.emit();
    }

    template<typename RuntimeResult, typename Operation>
    Result<RuntimeResult> submit(Operation&& operation)
    {
      submitting = true;
      auto const restoreSubmitting = gsl_lite::finally([this] { submitting = false; });
      auto result = std::invoke(std::forward<Operation>(operation));
      submitting = false;

      if (!result)
      {
        invalidate();
        return std::unexpected{result.error()};
      }

      if (result->status != rt::ListOrderAuthoringStatus::NoOp)
      {
        invalidate();
      }

      return result;
    }

    Result<rt::LibraryWriter::MoveOrderAuthoringResult> move(std::span<TrackId const> const selectedTrackIds,
                                                             std::optional<TrackId> const optBeforeTrackId)
    {
      return submit<rt::LibraryWriter::MoveOrderAuthoringResult>(
        [this, selectedTrackIds, optBeforeTrackId]
        { return library.writer().moveListOrder(order, selectedTrackIds, optBeforeTrackId); });
    }

    Result<std::optional<TrackId>> relativeAnchor(std::span<TrackId const> const selectedTrackIds,
                                                  int const direction) const
    {
      auto const effective = order.effectiveTrackIds();
      auto const effectiveMembership = std::unordered_set<TrackId>{effective.begin(), effective.end()};
      auto selectedMembership = std::unordered_set<TrackId>{};

      for (auto const trackId : selectedTrackIds)
      {
        if (!effectiveMembership.contains(trackId))
        {
          return makeError(Error::Code::InvalidInput, "Every selected track must belong to the bound List source");
        }

        selectedMembership.insert(trackId);
      }

      if (selectedMembership.empty())
      {
        return std::optional<TrackId>{};
      }

      auto remaining = std::vector<TrackId>{};
      remaining.reserve(effective.size() - selectedMembership.size());
      std::size_t unselectedBeforeFirst = 0;
      std::size_t unselectedBeforeLast = 0;
      bool sawSelected = false;

      for (auto const trackId : effective)
      {
        if (selectedMembership.contains(trackId))
        {
          if (!sawSelected)
          {
            unselectedBeforeFirst = remaining.size();
            sawSelected = true;
          }

          unselectedBeforeLast = remaining.size();
        }
        else
        {
          remaining.push_back(trackId);
        }
      }

      std::size_t insertionIndex = 0;

      if (direction < 0)
      {
        insertionIndex = unselectedBeforeFirst == 0 ? 0 : unselectedBeforeFirst - 1;
      }
      else
      {
        insertionIndex = std::min(remaining.size(), unselectedBeforeLast + 1);
      }

      return insertionIndex == remaining.size() ? std::optional<TrackId>{}
                                                : std::optional<TrackId>{remaining[insertionIndex]};
    }

    rt::Library& library;
    rt::ViewService& views;
    rt::ViewId viewId = rt::kInvalidViewId;
    rt::BoundListOrder order;
    ListOrderCapabilityState capabilities;
    std::shared_ptr<rt::TrackListProjection> projectionPtr;
    bool current = true;
    bool submitting = false;
    bool projectionEventsArmed = false;
    async::Subscription availabilitySubscription;
    async::Subscription presentationSubscription;
    async::Subscription projectionReplacementSubscription;
    async::Subscription viewDestroyedSubscription;
    async::Subscription projectionSubscription;
    mutable async::Signal<> invalidated;
  };

  Result<std::unique_ptr<ListOrderAuthoringSession>> ListOrderAuthoringSession::begin(rt::Library& library,
                                                                                      rt::ViewService& views,
                                                                                      rt::ViewId const viewId)
  {
    auto stateResult = views.findTrackListState(viewId);

    if (!stateResult)
    {
      return std::unexpected{stateResult.error()};
    }

    auto sourceStateResult = views.listSourceState(viewId);

    if (!sourceStateResult)
    {
      return std::unexpected{sourceStateResult.error()};
    }

    auto capabilities = describeListOrderCapabilities(ListOrderCapabilityInput{
      .listId = stateResult->listId,
      .presentation = stateResult->presentation,
      .quickFilterExpression = stateResult->filterExpression,
      .sourceLive = *sourceStateResult == rt::TrackSourceState::Live,
      .sourceHasError = stateResult->optFilterError.has_value(),
      .authoring = library.authoringAvailability(),
    });

    if (!capabilities.canAuthorOrder)
    {
      return makeError(Error::Code::InvalidState, capabilities.disabledReason);
    }

    auto sourceTrackIdsResult = views.listSourceTrackIds(viewId);

    if (!sourceTrackIdsResult)
    {
      return std::unexpected{sourceTrackIdsResult.error()};
    }

    auto orderResult = library.bindListOrder(stateResult->listId, std::move(*sourceTrackIdsResult));

    if (!orderResult)
    {
      return std::unexpected{orderResult.error()};
    }

    auto projectionResult = views.findTrackListProjection(viewId);

    if (!projectionResult)
    {
      return std::unexpected{projectionResult.error()};
    }

    return std::unique_ptr<ListOrderAuthoringSession>{new ListOrderAuthoringSession{std::make_unique<Impl>(
      library, views, viewId, std::move(*orderResult), std::move(capabilities), std::move(*projectionResult))}};
  }

  ListOrderAuthoringSession::ListOrderAuthoringSession(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  ListOrderAuthoringSession::~ListOrderAuthoringSession() = default;

  bool ListOrderAuthoringSession::isCurrent() const noexcept
  {
    return _implPtr->current;
  }

  ListOrderCapabilityState const& ListOrderAuthoringSession::capabilities() const noexcept
  {
    return _implPtr->capabilities;
  }

  std::span<TrackId const> ListOrderAuthoringSession::effectiveTrackIds() const noexcept
  {
    return _implPtr->order.effectiveTrackIds();
  }

  async::Subscription ListOrderAuthoringSession::onInvalidated(std::move_only_function<void() noexcept> handler) const
  {
    return _implPtr->invalidated.connect(std::move(handler));
  }

  Result<rt::LibraryWriter::MoveOrderAuthoringResult> ListOrderAuthoringSession::moveBefore(
    std::span<TrackId const> const selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::MoveOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canGapMove)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    return _implPtr->move(selectedTrackIds, optBeforeTrackId);
  }

  Result<rt::LibraryWriter::MoveOrderAuthoringResult> ListOrderAuthoringSession::moveUp(
    std::span<TrackId const> const selectedTrackIds)
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::MoveOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canRelativeMove)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    auto anchorResult = _implPtr->relativeAnchor(selectedTrackIds, -1);
    return anchorResult ? _implPtr->move(selectedTrackIds, *anchorResult) : std::unexpected{anchorResult.error()};
  }

  Result<rt::LibraryWriter::MoveOrderAuthoringResult> ListOrderAuthoringSession::moveDown(
    std::span<TrackId const> const selectedTrackIds)
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::MoveOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canRelativeMove)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    auto anchorResult = _implPtr->relativeAnchor(selectedTrackIds, 1);
    return anchorResult ? _implPtr->move(selectedTrackIds, *anchorResult) : std::unexpected{anchorResult.error()};
  }

  Result<rt::LibraryWriter::MoveOrderAuthoringResult> ListOrderAuthoringSession::moveToTop(
    std::span<TrackId const> const selectedTrackIds)
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::MoveOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canAbsoluteMove)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    auto const selectedMembership = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};
    auto optBeforeTrackId = std::optional<TrackId>{};

    for (auto const trackId : _implPtr->order.effectiveTrackIds())
    {
      if (!selectedMembership.contains(trackId))
      {
        optBeforeTrackId = trackId;
        break;
      }
    }

    return _implPtr->move(selectedTrackIds, optBeforeTrackId);
  }

  Result<rt::LibraryWriter::MoveOrderAuthoringResult> ListOrderAuthoringSession::moveToBottom(
    std::span<TrackId const> const selectedTrackIds)
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::MoveOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canAbsoluteMove)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    return _implPtr->move(selectedTrackIds, std::nullopt);
  }

  Result<rt::LibraryWriter::ResetOrderAuthoringResult> ListOrderAuthoringSession::resetOrder()
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::ResetOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canResetOrder)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    return _implPtr->submit<rt::LibraryWriter::ResetOrderAuthoringResult>(
      [this] { return _implPtr->library.writer().resetListOrder(_implPtr->order); });
  }

  Result<rt::LibraryWriter::ForgetHiddenOrderAuthoringResult> ListOrderAuthoringSession::forgetHiddenPositions()
  {
    if (!_implPtr->current)
    {
      return rt::LibraryWriter::ForgetHiddenOrderAuthoringResult{.status = rt::ListOrderAuthoringStatus::Stale};
    }

    if (!_implPtr->capabilities.canForgetHiddenPositions)
    {
      return makeError(Error::Code::InvalidState, _implPtr->capabilities.disabledReason);
    }

    return _implPtr->submit<rt::LibraryWriter::ForgetHiddenOrderAuthoringResult>(
      [this] { return _implPtr->library.writer().forgetHiddenListOrder(_implPtr->order); });
  }
} // namespace ao::uimodel
