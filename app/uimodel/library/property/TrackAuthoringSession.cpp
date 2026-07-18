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
  namespace
  {
    TrackAuthoringSubmitStatus mapStatus(rt::TrackAuthoringStatus status)
    {
      switch (status)
      {
        case rt::TrackAuthoringStatus::Applied: return TrackAuthoringSubmitStatus::Applied;
        case rt::TrackAuthoringStatus::NoOp: return TrackAuthoringSubmitStatus::NoOp;
        case rt::TrackAuthoringStatus::Stale: return TrackAuthoringSubmitStatus::Stale;
        case rt::TrackAuthoringStatus::Missing: return TrackAuthoringSubmitStatus::Missing;
        case rt::TrackAuthoringStatus::Unavailable: return TrackAuthoringSubmitStatus::Unavailable;
      }

      return TrackAuthoringSubmitStatus::Unavailable;
    }
  } // namespace

  struct TrackAuthoringSession::Impl final
  {
    Impl(rt::Library& libraryValue, rt::BoundTrackTargets targetsValue)
      : library{libraryValue}, targets{std::move(targetsValue)}
    {
      availabilitySubscription = library.onAuthoringAvailabilityChanged(
        [this](rt::LibraryAuthoringAvailability const& availability) { handleAvailability(availability); });
      handleAvailability(library.authoringAvailability());
    }

    bool canSubmit() const noexcept
    {
      return currentState == TrackAuthoringSessionState::Editing || currentState == TrackAuthoringSessionState::Applied;
    }

    bool bindingIsCurrent() const
    {
      auto const availability = library.authoringAvailability();
      return availability.state == rt::LibraryAuthoringState::Available &&
             availability.runtimeInstanceId == targets.runtimeInstanceId() &&
             availability.libraryRevision == targets.libraryRevision();
    }

    void setState(TrackAuthoringSessionState nextState)
    {
      if (currentState == nextState)
      {
        return;
      }

      currentState = nextState;
      stateChanged.emit(currentState);
    }

    void handleAvailability(rt::LibraryAuthoringAvailability const& availability)
    {
      if (currentState == TrackAuthoringSessionState::Submitting ||
          currentState == TrackAuthoringSessionState::Rejected || currentState == TrackAuthoringSessionState::Stale)
      {
        return;
      }

      if (availability.state != rt::LibraryAuthoringState::Available ||
          availability.runtimeInstanceId != targets.runtimeInstanceId() ||
          availability.libraryRevision != targets.libraryRevision())
      {
        setState(TrackAuthoringSessionState::Stale);
      }
    }

    template<typename RuntimeOutcome, typename SubmitResult>
    Result<SubmitResult> finishSubmission(Result<RuntimeOutcome> outcomeResult)
    {
      if (!outcomeResult)
      {
        setState(bindingIsCurrent() ? TrackAuthoringSessionState::Rejected : TrackAuthoringSessionState::Stale);
        return std::unexpected{outcomeResult.error()};
      }

      auto outcome = std::move(*outcomeResult);
      auto result = SubmitResult{
        .status = mapStatus(outcome.status),
        .reply = std::move(outcome.reply),
        .missingTargetIds = std::move(outcome.missingTargetIds),
      };

      switch (outcome.status)
      {
        case rt::TrackAuthoringStatus::Applied:
          if (!outcome.optNextTargets)
          {
            setState(TrackAuthoringSessionState::Rejected);
            return makeError(Error::Code::InvalidState, "Applied authoring result did not return a next binding");
          }

          targets = std::move(*outcome.optNextTargets);
          setState(bindingIsCurrent() ? TrackAuthoringSessionState::Applied : TrackAuthoringSessionState::Stale);
          break;
        case rt::TrackAuthoringStatus::NoOp:
          setState(bindingIsCurrent() ? TrackAuthoringSessionState::Editing : TrackAuthoringSessionState::Stale);
          break;
        case rt::TrackAuthoringStatus::Stale:
        case rt::TrackAuthoringStatus::Unavailable: setState(TrackAuthoringSessionState::Stale); break;
        case rt::TrackAuthoringStatus::Missing: setState(TrackAuthoringSessionState::Rejected); break;
      }

      return result;
    }

    void finishExceptionalSubmission() noexcept
    {
      try
      {
        setState(bindingIsCurrent() ? TrackAuthoringSessionState::Rejected : TrackAuthoringSessionState::Stale);
      }
      catch (...)
      {
        // Preserve the submission exception; setState changes state before notification.
        return;
      }
    }

    template<typename RuntimeOutcome, typename SubmitResult, typename Operation>
    Result<SubmitResult> runSubmission(Operation&& operation)
    {
      try
      {
        setState(TrackAuthoringSessionState::Submitting);
        return finishSubmission<RuntimeOutcome, SubmitResult>(std::invoke(std::forward<Operation>(operation)));
      }
      catch (...)
      {
        finishExceptionalSubmission();
        throw;
      }
    }

    rt::Library& library;
    rt::BoundTrackTargets targets;
    TrackAuthoringSessionState currentState = TrackAuthoringSessionState::Editing;
    async::Subscription availabilitySubscription;
    mutable async::Signal<TrackAuthoringSessionState> stateChanged;
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

  TrackAuthoringSessionState TrackAuthoringSession::state() const noexcept
  {
    return _implPtr->currentState;
  }

  std::span<TrackId const> TrackAuthoringSession::targetIds() const noexcept
  {
    return _implPtr->targets.trackIds();
  }

  async::Subscription TrackAuthoringSession::onStateChanged(
    std::move_only_function<void(TrackAuthoringSessionState)> handler) const
  {
    return _implPtr->stateChanged.connect(std::move(handler));
  }

  Result<TrackMetadataSubmitResult> TrackAuthoringSession::submitMetadata(rt::MetadataPatch const& patch)
  {
    if (!_implPtr->canSubmit())
    {
      return TrackMetadataSubmitResult{.status = _implPtr->currentState == TrackAuthoringSessionState::Stale
                                                   ? TrackAuthoringSubmitStatus::Stale
                                                   : TrackAuthoringSubmitStatus::Unavailable};
    }

    return _implPtr->runSubmission<rt::LibraryWriter::MetadataAuthoringOutcome, TrackMetadataSubmitResult>(
      [this, &patch] { return _implPtr->library.writer().updateMetadata(_implPtr->targets, patch); });
  }

  Result<TrackTagSubmitResult> TrackAuthoringSession::submitTags(std::span<std::string const> tagsToAdd,
                                                                 std::span<std::string const> tagsToRemove)
  {
    if (!_implPtr->canSubmit())
    {
      return TrackTagSubmitResult{.status = _implPtr->currentState == TrackAuthoringSessionState::Stale
                                              ? TrackAuthoringSubmitStatus::Stale
                                              : TrackAuthoringSubmitStatus::Unavailable};
    }

    return _implPtr->runSubmission<rt::LibraryWriter::TagAuthoringOutcome, TrackTagSubmitResult>(
      [this, tagsToAdd, tagsToRemove]
      { return _implPtr->library.writer().editTags(_implPtr->targets, tagsToAdd, tagsToRemove); });
  }
} // namespace ao::uimodel
