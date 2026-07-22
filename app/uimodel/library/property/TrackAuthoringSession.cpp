// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

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

    bool bindingIsCurrent() const
    {
      auto const availability = library.authoringAvailability();
      return availability.state == rt::LibraryAuthoringState::Available &&
             availability.runtimeInstanceId == targets.runtimeInstanceId() &&
             availability.libraryRevision == targets.libraryRevision();
    }

    void invalidate(rt::TrackAuthoringStatus const nextStatus)
    {
      if (!current || submitting)
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

      if (availability.state != rt::LibraryAuthoringState::Available ||
          availability.runtimeInstanceId != targets.runtimeInstanceId() ||
          availability.libraryRevision != targets.libraryRevision())
      {
        invalidate(rt::TrackAuthoringStatus::Stale);
      }
    }

    template<typename RuntimeResult, typename SubmitResult>
    Result<SubmitResult> finishSubmission(Result<RuntimeResult> runtimeResult)
    {
      if (!runtimeResult)
      {
        invalidate(bindingIsCurrent() ? rt::TrackAuthoringStatus::Unavailable : rt::TrackAuthoringStatus::Stale);
        return std::unexpected{runtimeResult.error()};
      }

      auto completed = std::move(*runtimeResult);
      auto result = SubmitResult{.status = completed.status, .reply = std::move(completed.reply)};

      switch (completed.status)
      {
        case rt::TrackAuthoringStatus::Applied:
          if (!completed.optNextTargets)
          {
            invalidate(rt::TrackAuthoringStatus::Unavailable);
            return makeError(Error::Code::InvalidState, "Applied authoring result did not return a next binding");
          }

          targets = std::move(*completed.optNextTargets);

          if (!bindingIsCurrent())
          {
            invalidate(rt::TrackAuthoringStatus::Stale);
          }

          break;
        case rt::TrackAuthoringStatus::NoOp:
          if (!bindingIsCurrent())
          {
            invalidate(rt::TrackAuthoringStatus::Stale);
          }

          break;
        case rt::TrackAuthoringStatus::Stale:
        case rt::TrackAuthoringStatus::Unavailable: invalidate(rt::TrackAuthoringStatus::Stale); break;
        case rt::TrackAuthoringStatus::Missing: invalidate(rt::TrackAuthoringStatus::Unavailable); break;
      }

      return result;
    }

    void finishExceptionalSubmission() noexcept
    {
      try
      {
        invalidate(bindingIsCurrent() ? rt::TrackAuthoringStatus::Unavailable : rt::TrackAuthoringStatus::Stale);
      }
      catch (...)
      {
        // Preserve the submission exception; invalidate changes state before notification.
        return;
      }
    }

    template<typename RuntimeResult, typename SubmitResult, typename Operation>
    Result<SubmitResult> runSubmission(Operation&& operation)
    {
      submitting = true;

      try
      {
        auto runtimeResult = std::invoke(std::forward<Operation>(operation));
        submitting = false;
        return finishSubmission<RuntimeResult, SubmitResult>(std::move(runtimeResult));
      }
      catch (...)
      {
        submitting = false;
        finishExceptionalSubmission();
        throw;
      }
    }

    rt::Library& library;
    rt::BoundTrackTargets targets;
    bool current = true;
    bool submitting = false;
    rt::TrackAuthoringStatus invalidStatus = rt::TrackAuthoringStatus::Unavailable;
    async::Subscription availabilitySubscription;
    mutable async::Signal<> invalidated;
  };

  Result<std::unique_ptr<TrackAuthoringSession>> TrackAuthoringSession::begin(rt::Library& library,
                                                                              std::span<TrackId const> targetIds)
  {
    auto targetsResult = library.bindTrackTargets(targetIds);

    if (!targetsResult)
    {
      return std::unexpected{targetsResult.error()};
    }

    return std::unique_ptr<TrackAuthoringSession>{
      new TrackAuthoringSession{std::make_unique<Impl>(library, std::move(*targetsResult))}};
  }

  TrackAuthoringSession::TrackAuthoringSession(std::unique_ptr<Impl> implPtr)
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

  async::Subscription TrackAuthoringSession::onInvalidated(std::move_only_function<void()> handler) const
  {
    return _implPtr->invalidated.connect(std::move(handler));
  }

  Result<TrackMetadataSubmitResult> TrackAuthoringSession::submitMetadata(rt::MetadataPatch const& patch)
  {
    if (!_implPtr->current)
    {
      return TrackMetadataSubmitResult{.status = _implPtr->invalidStatus};
    }

    return _implPtr->runSubmission<rt::LibraryWriter::MetadataAuthoringResult, TrackMetadataSubmitResult>(
      [this, &patch] { return _implPtr->library.writer().updateMetadata(_implPtr->targets, patch); });
  }

  Result<TrackTagSubmitResult> TrackAuthoringSession::submitTags(std::span<std::string const> tagsToAdd,
                                                                 std::span<std::string const> tagsToRemove)
  {
    if (!_implPtr->current)
    {
      return TrackTagSubmitResult{.status = _implPtr->invalidStatus};
    }

    return _implPtr->runSubmission<rt::LibraryWriter::TagAuthoringResult, TrackTagSubmitResult>(
      [this, tagsToAdd, tagsToRemove]
      { return _implPtr->library.writer().editTags(_implPtr->targets, tagsToAdd, tagsToRemove); });
  }
} // namespace ao::uimodel
