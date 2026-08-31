// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListOrderSession.h>

#include <algorithm>
#include <cstddef>
#include <exception>
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
  struct ListOrderAuthoringSession::State final
  {
    State(rt::Library& libraryValue,
          rt::ViewService& viewsValue,
          rt::ViewId const viewIdValue,
          rt::BoundListOrder orderValue,
          ListOrderCapabilityState capabilitiesValue,
          std::shared_ptr<rt::TrackListProjection const> projectionValuePtr,
          i18n::MessageCatalog textCatalogValue)
      : library{libraryValue}
      , views{viewsValue}
      , viewId{viewIdValue}
      , order{std::move(orderValue)}
      , capabilities{std::move(capabilitiesValue)}
      , projectionPtr{std::move(projectionValuePtr)}
      , textCatalog{std::move(textCatalogValue)}
    {
      availabilitySubscription = library.onAuthoringAvailabilityChanged(
        [this](rt::LibraryAuthoringAvailability const& availability)
        {
          if (availability.state == rt::LibraryAuthoringState::Maintenance)
          {
            capabilities.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderLibraryBusy);
          }

          if (!order.matches(availability))
          {
            invalidate();
          }
        });
      presentationSubscription = views.onPresentationChanged(
        [this](rt::ViewService::PresentationChanged const& event)
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      projectionReplacementSubscription = views.onProjectionChanged(
        [this](rt::TrackListProjectionChanged const& event)
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      viewDestroyedSubscription = views.onViewDestroyed(
        [this](rt::ViewService::ViewDestroyed const& event)
        {
          if (event.viewId == viewId)
          {
            invalidate();
          }
        });
      projectionSubscription = projectionPtr->subscribe(
        [this](rt::TrackListProjectionDeltaBatch const&)
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
      if (!current)
      {
        return;
      }

      if (submitting)
      {
        invalidationPending = true;
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
        capabilities.disabledReason = i18n::requiredText(textCatalog, i18n::MessageId::ListOrderChanged);
      }

      invalidated.emit();
    }

    void reconcileAfterSubmission(bool const operationInvalidates)
    {
      auto const pending = std::exchange(invalidationPending, false);

      if (operationInvalidates || pending || !order.matches(library.authoringAvailability()))
      {
        invalidate();
      }
    }

    void reconcileExceptionalSubmission() noexcept
    {
      try
      {
        reconcileAfterSubmission(false);
      }
      catch (...)
      {
        AO_AUDITED_CATCH(PreservePrimaryException);
        // Preserve the submission exception after recording any pending invalidation.
      }
    }

    template<typename Reply>
    std::optional<Result<rt::AuthoringResult<Reply>>> commandRejection(bool const enabled) const
    {
      if (!current)
      {
        return Result<rt::AuthoringResult<Reply>>{rt::AuthoringResult<Reply>{.status = rt::AuthoringStatus::Stale}};
      }

      if (!enabled)
      {
        return Result<rt::AuthoringResult<Reply>>{makeError(Error::Code::InvalidState, capabilities.disabledReason)};
      }

      return std::nullopt;
    }

    template<typename RuntimeResult, typename Operation>
    static async::Task<Result<RuntimeResult>> submitAsync(std::shared_ptr<State> statePtr, Operation operation)
    {
      if (!statePtr->current)
      {
        co_return RuntimeResult{.status = rt::AuthoringStatus::Stale};
      }

      if (statePtr->submitting)
      {
        co_return RuntimeResult{.status = rt::AuthoringStatus::Busy};
      }

      statePtr->submitting = true;
      auto result = Result<RuntimeResult>{};
      auto deferredException = std::exception_ptr{};

      try
      {
        result = co_await std::invoke(std::move(operation), *statePtr);
      }
      catch (...)
      {
        deferredException = std::current_exception();
        statePtr->submitting = false;
        statePtr->reconcileExceptionalSubmission();
        async::rethrowException(deferredException);
      }

      statePtr->submitting = false;

      if (!result)
      {
        statePtr->reconcileAfterSubmission(true);
        co_return std::unexpected{result.error()};
      }

      statePtr->reconcileAfterSubmission(result->status != rt::AuthoringStatus::NoOp &&
                                         result->status != rt::AuthoringStatus::Busy);

      co_return result;
    }

    static async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveAsync(
      std::shared_ptr<State> statePtr,
      std::vector<TrackId> selectedTrackIds,
      std::optional<TrackId> optBeforeTrackId)
    {
      return submitAsync<rt::AuthoringResult<rt::MoveListOrderReply>>(
        std::move(statePtr),
        [selectedTrackIds = std::move(selectedTrackIds), optBeforeTrackId](State& state) mutable
        { return state.library.commands().moveListOrder(state.order, std::move(selectedTrackIds), optBeforeTrackId); });
    }

    static async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>>
    moveRelative(std::shared_ptr<State> statePtr, std::vector<TrackId> selectedTrackIds, int const direction)
    {
      if (auto optRejection =
            statePtr->commandRejection<rt::MoveListOrderReply>(statePtr->capabilities.canRelativeMove);
          optRejection)
      {
        return async::makeReadyTask(std::move(*optRejection));
      }

      auto anchorRes = statePtr->relativeAnchor(selectedTrackIds, direction);

      if (!anchorRes)
      {
        return async::makeReadyTask(
          Result<rt::AuthoringResult<rt::MoveListOrderReply>>{std::unexpected{anchorRes.error()}});
      }

      return moveAsync(std::move(statePtr), std::move(selectedTrackIds), *anchorRes);
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
    std::shared_ptr<rt::TrackListProjection const> projectionPtr;
    i18n::MessageCatalog textCatalog;
    bool current = true;
    bool submitting = false;
    bool invalidationPending = false;
    bool projectionEventsArmed = false;
    async::Subscription availabilitySubscription;
    async::Subscription presentationSubscription;
    async::Subscription projectionReplacementSubscription;
    async::Subscription viewDestroyedSubscription;
    async::Subscription projectionSubscription;
    mutable async::Signal<> invalidated;
  };

  Result<ListOrderAuthoringSession> ListOrderAuthoringSession::begin(rt::Library& library,
                                                                     rt::ViewService& views,
                                                                     rt::ViewId const viewId,
                                                                     i18n::MessageCatalog const& textCatalog)
  {
    auto stateRes = views.findTrackListState(viewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    auto sourceStateRes = views.listSourceState(viewId);

    if (!sourceStateRes)
    {
      return std::unexpected{sourceStateRes.error()};
    }

    auto capabilities = describeListOrderCapabilities(textCatalog,
                                                      ListOrderCapabilityInput{
                                                        .listId = stateRes->listId,
                                                        .presentation = stateRes->presentation,
                                                        .quickFilterExpression = stateRes->filterExpression,
                                                        .sourceLive = *sourceStateRes == rt::TrackSourceState::Live,
                                                        .sourceHasError = stateRes->optFilterError.has_value(),
                                                        .authoring = library.authoringAvailability(),
                                                      });

    if (!capabilities.canAuthorOrder)
    {
      return makeError(Error::Code::InvalidState, capabilities.disabledReason);
    }

    auto sourceTrackIdsRes = views.listSourceTrackIds(viewId);

    if (!sourceTrackIdsRes)
    {
      return std::unexpected{sourceTrackIdsRes.error()};
    }

    auto orderRes = library.bindListOrder(stateRes->listId, std::move(*sourceTrackIdsRes));

    if (!orderRes)
    {
      return std::unexpected{orderRes.error()};
    }

    auto projectionRes = views.findTrackListProjection(viewId);

    if (!projectionRes)
    {
      return std::unexpected{projectionRes.error()};
    }

    return ListOrderAuthoringSession{std::make_shared<State>(
      library, views, viewId, std::move(*orderRes), std::move(capabilities), std::move(*projectionRes), textCatalog)};
  }

  ListOrderAuthoringSession::ListOrderAuthoringSession(std::shared_ptr<State> statePtr)
    : _statePtr{std::move(statePtr)}
  {
  }

  ListOrderAuthoringSession::~ListOrderAuthoringSession() = default;

  ListOrderAuthoringSession::ListOrderAuthoringSession(ListOrderAuthoringSession&&) noexcept = default;

  bool ListOrderAuthoringSession::isCurrent() const noexcept
  {
    return _statePtr->current;
  }

  ListOrderCapabilityState const& ListOrderAuthoringSession::capabilities() const noexcept
  {
    return _statePtr->capabilities;
  }

  std::span<TrackId const> ListOrderAuthoringSession::effectiveTrackIds() const noexcept
  {
    return _statePtr->order.effectiveTrackIds();
  }

  async::Subscription ListOrderAuthoringSession::onInvalidated(compat::MoveOnlyFunction<void()> handler) const
  {
    return _statePtr->invalidated.connect(std::move(handler));
  }

  async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> ListOrderAuthoringSession::moveBefore(
    std::vector<TrackId> selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    if (auto optRejection = _statePtr->commandRejection<rt::MoveListOrderReply>(_statePtr->capabilities.canGapMove);
        optRejection)
    {
      return async::makeReadyTask(std::move(*optRejection));
    }

    return State::moveAsync(_statePtr, std::move(selectedTrackIds), optBeforeTrackId);
  }

  async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> ListOrderAuthoringSession::moveUp(
    std::vector<TrackId> selectedTrackIds)
  {
    return State::moveRelative(_statePtr, std::move(selectedTrackIds), -1);
  }

  async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> ListOrderAuthoringSession::moveDown(
    std::vector<TrackId> selectedTrackIds)
  {
    return State::moveRelative(_statePtr, std::move(selectedTrackIds), 1);
  }

  async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> ListOrderAuthoringSession::moveToTop(
    std::vector<TrackId> selectedTrackIds)
  {
    if (auto optRejection =
          _statePtr->commandRejection<rt::MoveListOrderReply>(_statePtr->capabilities.canAbsoluteMove);
        optRejection)
    {
      return async::makeReadyTask(std::move(*optRejection));
    }

    auto const selectedMembership = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};
    auto optBeforeTrackId = std::optional<TrackId>{};

    for (auto const trackId : _statePtr->order.effectiveTrackIds())
    {
      if (!selectedMembership.contains(trackId))
      {
        optBeforeTrackId = trackId;
        break;
      }
    }

    return State::moveAsync(_statePtr, std::move(selectedTrackIds), optBeforeTrackId);
  }

  async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> ListOrderAuthoringSession::moveToBottom(
    std::vector<TrackId> selectedTrackIds)
  {
    if (auto optRejection =
          _statePtr->commandRejection<rt::MoveListOrderReply>(_statePtr->capabilities.canAbsoluteMove);
        optRejection)
    {
      return async::makeReadyTask(std::move(*optRejection));
    }

    return State::moveAsync(_statePtr, std::move(selectedTrackIds), std::nullopt);
  }

  async::Task<Result<rt::AuthoringResult<rt::ResetListOrderReply>>> ListOrderAuthoringSession::resetOrder()
  {
    if (auto optRejection = _statePtr->commandRejection<rt::ResetListOrderReply>(_statePtr->capabilities.canResetOrder);
        optRejection)
    {
      return async::makeReadyTask(std::move(*optRejection));
    }

    return State::submitAsync<rt::AuthoringResult<rt::ResetListOrderReply>>(
      _statePtr, [](State& state) { return state.library.commands().resetListOrder(state.order); });
  }

  async::Task<Result<rt::AuthoringResult<rt::ForgetHiddenListOrderReply>>>
  ListOrderAuthoringSession::forgetHiddenPositions()
  {
    if (auto optRejection =
          _statePtr->commandRejection<rt::ForgetHiddenListOrderReply>(_statePtr->capabilities.canForgetHiddenPositions);
        optRejection)
    {
      return async::makeReadyTask(std::move(*optRejection));
    }

    return State::submitAsync<rt::AuthoringResult<rt::ForgetHiddenListOrderReply>>(
      _statePtr, [](State& state) { return state.library.commands().forgetHiddenListOrder(state.order); });
  }
} // namespace ao::uimodel
