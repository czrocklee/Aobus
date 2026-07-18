// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryMutationService.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::uint64_t nextRuntimeInstanceId() noexcept
    {
      static auto nextId = std::atomic<std::uint64_t>{1};
      return nextId.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t currentLibraryRevision(library::MusicLibrary const& library)
    {
      auto transaction = library.readTransaction();
      return library.libraryRevision(transaction);
    }

    void logMaintenanceCompletionDispatchFailure() noexcept
    {
      try
      {
        APP_LOG_ERROR("Failed to publish faulted library maintenance completion");
      }
      catch (...)
      {
        // Logging cannot escape MaintenanceGuard's destructor.
        return;
      }
    }
  } // namespace

  LibraryMutationService::Mutation::Mutation(LibraryMutationService& owner,
                                             std::unique_lock<std::mutex> writerLock,
                                             library::WriteTransaction transaction)
    : _owner{&owner}, _writerLock{std::move(writerLock)}, _transaction{std::move(transaction)}
  {
  }

  LibraryMutationService::Mutation::~Mutation() = default;

  LibraryMutationService::Mutation::Mutation(Mutation&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}
    , _writerLock{std::move(other._writerLock)}
    , _transaction{std::move(other._transaction)}
    , _terminal{std::exchange(other._terminal, true)}
  {
  }

  library::WriteTransaction& LibraryMutationService::Mutation::transaction() noexcept
  {
    return _transaction;
  }

  Result<LibraryMutationService::CommitInfo> LibraryMutationService::Mutation::commit(LibraryChangeSet changeSet)
  {
    if (_owner == nullptr || _terminal)
    {
      return makeError(Error::Code::InvalidState, "Library mutation is already terminal");
    }

    return _owner->commit(*this, std::move(changeSet));
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(LibraryMutationService& owner,
                                                             std::uint64_t generation) noexcept
    : _owner{&owner}, _generation{generation}
  {
  }

  LibraryMutationService::MaintenanceGuard::~MaintenanceGuard()
  {
    if (_owner != nullptr)
    {
      _owner->finishMaintenance(_generation);
    }
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(MaintenanceGuard&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  LibraryMutationService::LibraryMutationService(async::Executor& callbackExecutor,
                                                 library::WritableMusicLibrary writableLibrary,
                                                 LibraryChanges& changes)
    : _callbackExecutor{callbackExecutor}
    , _writableLibrary{std::move(writableLibrary)}
    , _library{_writableLibrary.library()}
    , _changes{changes}
    , _runtimeInstanceId{nextRuntimeInstanceId()}
    , _lastCommittedRevision{currentLibraryRevision(_library)}
    , _availableRevision{_lastCommittedRevision}
  {
  }

  LibraryMutationService::~LibraryMutationService() = default;

  LibraryAuthoringAvailability LibraryMutationService::availability() const
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return availabilityLocked();
  }

  async::Subscription LibraryMutationService::onAvailabilityChanged(
    std::move_only_function<void(LibraryAuthoringAvailability const&)> handler) const
  {
    return _availabilityChanged.connect(std::move(handler));
  }

  Result<BoundTrackTargets> LibraryMutationService::bindTrackTargets(std::span<TrackId const> trackIds) const
  {
    if (trackIds.empty())
    {
      return makeError(Error::Code::InvalidInput, "Cannot bind an empty track target set");
    }

    auto transaction = _library.readTransaction();
    auto const revision = _library.libraryRevision(transaction);
    std::uint64_t runtimeInstanceId = 0;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_state != LibraryAuthoringState::Available || revision != _availableRevision)
      {
        return makeError(Error::Code::InvalidState, "Library authoring is unavailable");
      }

      runtimeInstanceId = _runtimeInstanceId;
    }

    auto reader = _library.tracks().reader(transaction);

    for (auto const trackId : trackIds)
    {
      if (trackId == kInvalidTrackId || !reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
      {
        return makeError(Error::Code::NotFound, std::format("Track authoring target not found: {}", trackId));
      }
    }

    return BoundTrackTargets{runtimeInstanceId, revision, std::vector<TrackId>{trackIds.begin(), trackIds.end()}};
  }

  BoundTrackTargets LibraryMutationService::advanceBoundTargets(BoundTrackTargets const& targets,
                                                                std::uint64_t revision) const
  {
    return BoundTrackTargets{
      _runtimeInstanceId, revision, std::vector<TrackId>{targets._trackIds.begin(), targets._trackIds.end()}};
  }

  Result<std::unique_lock<std::mutex>> LibraryMutationService::acquireWriter(LibraryAuthoringState requiredState,
                                                                             std::string_view operation)
  {
    while (true)
    {
      {
        auto stateLock = std::unique_lock{_stateMutex};

        if (_publicationInProgress)
        {
          if (_callbackExecutor.isCurrent())
          {
            return makeError(Error::Code::InvalidState,
                             std::format("{} cannot start reentrantly during library publication", operation));
          }

          _publicationCompleted.wait(stateLock, [this] { return !_publicationInProgress; });
        }

        if (_state != requiredState)
        {
          return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
        }
      }

      auto writerLock = std::unique_lock{_writerMutex};
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_publicationInProgress)
      {
        writerLock.unlock();
        continue;
      }

      if (_state != requiredState)
      {
        return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
      }

      return writerLock;
    }
  }

  Result<LibraryMutationService::Mutation> LibraryMutationService::beginInteractiveMutation()
  {
    auto writerLockResult = acquireWriter(LibraryAuthoringState::Available, "Library mutation");

    if (!writerLockResult)
    {
      return std::unexpected{writerLockResult.error()};
    }

    return Mutation{*this, std::move(*writerLockResult), _writableLibrary.writeTransaction()};
  }

  LibraryMutationService::AuthoringStart LibraryMutationService::beginAuthoringMutation(
    BoundTrackTargets const& targets)
  {
    if (targets._runtimeInstanceId != _runtimeInstanceId)
    {
      return AuthoringStart{.status = TrackAuthoringStatus::Stale};
    }

    auto writerLockResult = acquireWriter(LibraryAuthoringState::Available, "Track authoring");

    if (!writerLockResult)
    {
      return AuthoringStart{.status = TrackAuthoringStatus::Unavailable};
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (targets._runtimeInstanceId != _runtimeInstanceId || targets._libraryRevision != _availableRevision)
      {
        return AuthoringStart{.status = TrackAuthoringStatus::Stale};
      }
    }

    auto transaction = _writableLibrary.writeTransaction();
    auto reader = _library.tracks().reader(transaction);
    auto missingTargetIds = std::vector<TrackId>{};

    for (auto const trackId : targets._trackIds)
    {
      if (trackId == kInvalidTrackId || !reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
      {
        missingTargetIds.push_back(trackId);
      }
    }

    if (!missingTargetIds.empty())
    {
      return AuthoringStart{.status = TrackAuthoringStatus::Missing, .missingTargetIds = std::move(missingTargetIds)};
    }

    auto result = AuthoringStart{.status = TrackAuthoringStatus::NoOp};
    result.optMutation.emplace(Mutation{*this, std::move(*writerLockResult), std::move(transaction)});
    return result;
  }

  Result<LibraryMutationService::MaintenanceGuard> LibraryMutationService::beginMaintenance(LibraryMaintenanceKind kind)
  {
    if (kind == LibraryMaintenanceKind::None)
    {
      return makeError(Error::Code::InvalidInput, "Library maintenance requires an operation kind");
    }

    auto writerLockResult = acquireWriter(LibraryAuthoringState::Available, "Library maintenance");

    if (!writerLockResult)
    {
      return std::unexpected{writerLockResult.error()};
    }

    auto expected = LibraryAuthoringAvailability{};
    std::uint64_t generation = 0;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      _state = LibraryAuthoringState::Maintenance;
      _maintenanceKind = kind;
      generation = ++_maintenanceGeneration;
      expected = availabilityLocked();
    }

    writerLockResult->unlock();

    try
    {
      _callbackExecutor.dispatch([this, expected] { emitAvailability(expected); });
    }
    catch (...)
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_state == LibraryAuthoringState::Maintenance && generation == _maintenanceGeneration)
      {
        _state = LibraryAuthoringState::Faulted;
        _maintenanceKind = LibraryMaintenanceKind::None;
      }

      throw;
    }

    return MaintenanceGuard{*this, generation};
  }

  Result<LibraryMutationService::Mutation> LibraryMutationService::beginMaintenanceMutation(
    MaintenanceGuard const& guard)
  {
    auto writerLockResult = acquireWriter(LibraryAuthoringState::Maintenance, "Library maintenance mutation");

    if (!writerLockResult)
    {
      return std::unexpected{writerLockResult.error()};
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (guard._owner != this || guard._generation != _maintenanceGeneration)
      {
        return makeError(Error::Code::InvalidState, "Library maintenance session is no longer active");
      }
    }

    return Mutation{*this, std::move(*writerLockResult), _writableLibrary.writeTransaction()};
  }

  Result<LibraryMutationService::CommitInfo> LibraryMutationService::commit(Mutation& mutation,
                                                                            LibraryChangeSet changeSet)
  {
    if (mutation._owner != this || !mutation._writerLock.owns_lock() || mutation._terminal)
    {
      return makeError(Error::Code::InvalidState, "Library mutation does not belong to this service");
    }

    auto finishMutation = [&mutation]
    {
      mutation._terminal = true;
      // Store writers borrow the transaction wrapper and may remain in scope
      // until their owning operation returns. Finish the native transaction,
      // but retain its wrapper so those writers can observe the terminal state.
      mutation._transaction.abort();
      mutation._writerLock.unlock();
    };

    std::uint64_t revision = 0;

    try
    {
      revision = _library.libraryRevision(mutation._transaction);
    }
    catch (...)
    {
      finishMutation();
      throw;
    }

    auto optFaulted = std::optional<LibraryAuthoringAvailability>{};
    std::uint64_t expectedRevision = 0;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      expectedRevision = _lastCommittedRevision + 1U;

      if (revision != expectedRevision)
      {
        _state = LibraryAuthoringState::Faulted;
        _maintenanceKind = LibraryMaintenanceKind::None;
        optFaulted = availabilityLocked();
      }
    }

    if (optFaulted)
    {
      finishMutation();
      _callbackExecutor.dispatch([this, expected = *optFaulted] { emitAvailability(expected); });
      return makeError(
        Error::Code::InvalidState,
        std::format("Library revision gap before commit: expected {}, got {}", expectedRevision, revision));
    }

    auto commitResult = Result<>{};

    try
    {
      commitResult = mutation._transaction.commit();
    }
    catch (...)
    {
      finishMutation();
      throw;
    }

    if (!commitResult)
    {
      finishMutation();
      return std::unexpected{commitResult.error()};
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      _lastCommittedRevision = revision;
      _publicationInProgress = true;
    }

    finishMutation();
    changeSet.libraryRevision = revision;

    try
    {
      _changes.publishFromCoordinator(std::move(changeSet),
                                      [this, revision](std::exception_ptr failure)
                                      { handlePublication(revision, std::move(failure)); });
    }
    catch (...)
    {
      auto const failure = std::current_exception();
      bool publicationStillPending = false;

      {
        auto const stateLock = std::scoped_lock{_stateMutex};
        publicationStillPending = _publicationInProgress;
      }

      if (publicationStillPending)
      {
        try
        {
          handlePublication(revision, failure);
        }
        catch (...)
        {
          // Preserve the original publication failure after faulting state.
          std::rethrow_exception(failure);
        }
      }

      std::rethrow_exception(failure);
    }

    return CommitInfo{.libraryRevision = revision};
  }

  void LibraryMutationService::finishMaintenance(std::uint64_t generation) noexcept
  {
    auto writerLockResult = acquireWriter(LibraryAuthoringState::Maintenance, "Library maintenance completion");

    if (!writerLockResult)
    {
      auto optFaulted = std::optional<LibraryAuthoringAvailability>{};

      {
        auto const stateLock = std::scoped_lock{_stateMutex};

        if (_state == LibraryAuthoringState::Maintenance && generation == _maintenanceGeneration)
        {
          _state = LibraryAuthoringState::Faulted;
          _maintenanceKind = LibraryMaintenanceKind::None;
          optFaulted = availabilityLocked();
        }
      }

      if (optFaulted)
      {
        try
        {
          _callbackExecutor.dispatch([this, expected = *optFaulted] { emitAvailability(expected); });
        }
        catch (...)
        {
          logMaintenanceCompletionDispatchFailure();
        }
      }

      return;
    }

    auto expected = LibraryAuthoringAvailability{};

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_state != LibraryAuthoringState::Maintenance || generation != _maintenanceGeneration)
      {
        return;
      }

      _state = LibraryAuthoringState::Available;
      _maintenanceKind = LibraryMaintenanceKind::None;
      expected = availabilityLocked();
    }

    writerLockResult->unlock();

    try
    {
      _callbackExecutor.dispatch([this, expected] { emitAvailability(expected); });
    }
    catch (...)
    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      _state = LibraryAuthoringState::Faulted;
      _maintenanceKind = LibraryMaintenanceKind::None;
    }
  }

  void LibraryMutationService::handlePublication(std::uint64_t revision, std::exception_ptr failure)
  {
    auto expected = LibraryAuthoringAvailability{};
    bool shouldEmit = false;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (failure)
      {
        _state = LibraryAuthoringState::Faulted;
        _maintenanceKind = LibraryMaintenanceKind::None;
        expected = availabilityLocked();
        shouldEmit = true;
      }
      else
      {
        _availableRevision = revision;

        if (_state == LibraryAuthoringState::Available && _lastCommittedRevision == revision)
        {
          expected = availabilityLocked();
          shouldEmit = true;
        }
      }
    }

    auto completionFailure = failure;

    if (shouldEmit)
    {
      try
      {
        if (_callbackExecutor.isCurrent())
        {
          emitAvailability(expected);
        }
        else
        {
          _callbackExecutor.dispatch([this, expected] { emitAvailability(expected); });
        }
      }
      catch (...)
      {
        if (!completionFailure)
        {
          completionFailure = std::current_exception();
        }

        auto const stateLock = std::scoped_lock{_stateMutex};
        _state = LibraryAuthoringState::Faulted;
        _maintenanceKind = LibraryMaintenanceKind::None;
      }
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      _publicationInProgress = false;
    }

    _publicationCompleted.notify_all();

    if (completionFailure)
    {
      std::rethrow_exception(completionFailure);
    }
  }

  void LibraryMutationService::emitAvailability(LibraryAuthoringAvailability const& expected)
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (availabilityLocked() != expected)
      {
        return;
      }
    }

    try
    {
      _availabilityChanged.emit(expected);
    }
    catch (...)
    {
      auto const failure = std::current_exception();
      auto faulted = LibraryAuthoringAvailability{};

      {
        auto const stateLock = std::scoped_lock{_stateMutex};
        _state = LibraryAuthoringState::Faulted;
        _maintenanceKind = LibraryMaintenanceKind::None;
        faulted = availabilityLocked();
      }

      if (expected.state != LibraryAuthoringState::Faulted)
      {
        try
        {
          _availabilityChanged.emit(faulted);
        }
        catch (...)
        {
          // Fault notification is best effort; preserve the first observer failure.
          std::rethrow_exception(failure);
        }
      }

      std::rethrow_exception(failure);
    }
  }

  LibraryAuthoringAvailability LibraryMutationService::availabilityLocked() const noexcept
  {
    auto const revision = _state == LibraryAuthoringState::Faulted ? _lastCommittedRevision : _availableRevision;
    return LibraryAuthoringAvailability{.state = _state,
                                        .runtimeInstanceId = _runtimeInstanceId,
                                        .libraryRevision = revision,
                                        .maintenanceKind = _state == LibraryAuthoringState::Maintenance
                                                             ? _maintenanceKind
                                                             : LibraryMaintenanceKind::None};
  }
} // namespace ao::rt
