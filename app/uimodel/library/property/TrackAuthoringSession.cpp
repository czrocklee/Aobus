// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>

#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  struct TrackAuthoringSession::Impl final
  {
    Impl(rt::Library& libraryValue, rt::BoundTrackTargets targetsValue)
      : library{libraryValue}, targets{std::move(targetsValue)}
    {
      availabilitySubscription = library.onAuthoringAvailabilityChanged(
        [this](rt::LibraryAuthoringAvailability const& availability) { handleAvailability(availability); });
      handleAvailability(library.authoringAvailability());
    }

    bool bindingIsCurrent() const { return targets.matches(library.authoringAvailability()); }

    void invalidate(rt::AuthoringStatus const nextStatus)
    {
      if (!current)
      {
        return;
      }

      current = false;
      invalidStatus = nextStatus;
      invalidated.emit();
    }

    void handleAvailability(rt::LibraryAuthoringAvailability const& availability)
    {
      if (!current)
      {
        return;
      }

      if (!targets.matches(availability))
      {
        if (submitting)
        {
          maintenanceObservedDuringSubmission =
            maintenanceObservedDuringSubmission || availability.state == rt::LibraryAuthoringState::Maintenance;
          return;
        }

        invalidate(rt::AuthoringStatus::Stale);
      }
    }

    bool bindingInvalidAfterSubmission()
    {
      auto const maintenanceObserved = std::exchange(maintenanceObservedDuringSubmission, false);
      return maintenanceObserved || !bindingIsCurrent();
    }

    template<typename RuntimeResult, typename SubmitResult>
    Result<SubmitResult> finishSubmission(Result<RuntimeResult> runtimeRes)
    {
      if (!runtimeRes)
      {
        invalidate(bindingInvalidAfterSubmission() ? rt::AuthoringStatus::Stale : rt::AuthoringStatus::Unavailable);
        return std::unexpected{runtimeRes.error()};
      }

      auto completed = std::move(*runtimeRes);
      auto result = SubmitResult{.status = completed.status, .reply = std::move(completed.reply)};

      switch (completed.status)
      {
        case rt::AuthoringStatus::Applied:
          AO_INVARIANT(completed.optNextTargets, "Applied authoring result did not return a next binding");

          targets = std::move(*completed.optNextTargets);
          break;
        case rt::AuthoringStatus::NoOp:
        case rt::AuthoringStatus::Busy: break;
        case rt::AuthoringStatus::Stale:
        case rt::AuthoringStatus::Unavailable: invalidate(rt::AuthoringStatus::Stale); break;
      }

      if (current && bindingInvalidAfterSubmission())
      {
        invalidate(rt::AuthoringStatus::Stale);
      }

      return result;
    }

    void finishExceptionalSubmission() noexcept
    {
      try
      {
        invalidate(bindingInvalidAfterSubmission() ? rt::AuthoringStatus::Stale : rt::AuthoringStatus::Unavailable);
      }
      catch (...)
      {
        AO_AUDITED_CATCH(PreservePrimaryException);
        // Preserve the submission exception; invalidate changes state before notification.
        return;
      }
    }

    template<typename RuntimeResult, typename SubmitResult, typename Operation>
    static async::Task<Result<SubmitResult>> runSubmissionAsync(std::shared_ptr<Impl> implPtr, Operation operation)
    {
      if (!implPtr->current)
      {
        co_return SubmitResult{.status = implPtr->invalidStatus};
      }

      if (implPtr->submitting)
      {
        co_return SubmitResult{.status = rt::AuthoringStatus::Busy};
      }

      implPtr->submitting = true;
      auto deferredException = std::exception_ptr{};

      try
      {
        auto runtimeRes = co_await std::invoke(std::move(operation), *implPtr);
        implPtr->submitting = false;
        co_return implPtr->finishSubmission<RuntimeResult, SubmitResult>(std::move(runtimeRes));
      }
      catch (...)
      {
        deferredException = std::current_exception();
        implPtr->submitting = false;
        implPtr->finishExceptionalSubmission();
        async::rethrowException(deferredException);
      }
    }

    rt::Library& library;
    rt::BoundTrackTargets targets;
    bool current = true;
    bool submitting = false;
    bool maintenanceObservedDuringSubmission = false;
    rt::AuthoringStatus invalidStatus = rt::AuthoringStatus::Unavailable;
    async::Subscription availabilitySubscription;
    mutable async::Signal<> invalidated;
  };

  Result<std::unique_ptr<TrackAuthoringSession>> TrackAuthoringSession::begin(rt::Library& library,
                                                                              std::span<TrackId const> targetIds)
  {
    auto targetsRes = library.bindTrackTargets(targetIds);

    if (!targetsRes)
    {
      return std::unexpected{targetsRes.error()};
    }

    return std::unique_ptr<TrackAuthoringSession>{
      new TrackAuthoringSession{std::make_shared<Impl>(library, std::move(*targetsRes))}};
  }

  TrackAuthoringSession::TrackAuthoringSession(std::shared_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  TrackAuthoringSession::~TrackAuthoringSession() = default;

  bool TrackAuthoringSession::isCurrent() const noexcept
  {
    return _implPtr->current;
  }

  std::span<TrackId const> TrackAuthoringSession::targetIds() const noexcept
  {
    return _implPtr->targets.trackIds();
  }

  async::Subscription TrackAuthoringSession::onInvalidated(compat::MoveOnlyFunction<void()> handler) const
  {
    return _implPtr->invalidated.connect(std::move(handler));
  }

  async::Task<Result<TrackMetadataSubmitResult>> TrackAuthoringSession::submitMetadata(rt::MetadataPatch patch)
  {
    return Impl::runSubmissionAsync<rt::TrackAuthoringResult<rt::UpdateTrackMetadataReply>, TrackMetadataSubmitResult>(
      _implPtr,
      [patch = std::move(patch)](Impl& impl) mutable
      { return impl.library.writer().updateMetadata(impl.targets, std::move(patch)); });
  }

  async::Task<Result<TrackTagSubmitResult>> TrackAuthoringSession::submitTags(std::vector<std::string> tagsToAdd,
                                                                              std::vector<std::string> tagsToRemove)
  {
    return Impl::runSubmissionAsync<rt::TrackAuthoringResult<rt::EditTrackTagsReply>, TrackTagSubmitResult>(
      _implPtr,
      [tagsToAdd = std::move(tagsToAdd), tagsToRemove = std::move(tagsToRemove)](Impl& impl) mutable
      { return impl.library.writer().editTags(impl.targets, std::move(tagsToAdd), std::move(tagsToRemove)); });
  }

  async::Task<Result<TrackPropertiesSubmitResult>> TrackAuthoringSession::submitProperties(
    rt::TrackPropertiesPatch patch)
  {
    return Impl::runSubmissionAsync<rt::TrackAuthoringResult<rt::UpdateTrackPropertiesReply>,
                                    TrackPropertiesSubmitResult>(
      _implPtr,
      [patch = std::move(patch)](Impl& impl) mutable
      { return impl.library.writer().updateProperties(impl.targets, std::move(patch)); });
  }
} // namespace ao::uimodel
