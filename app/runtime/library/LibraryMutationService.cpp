// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryMutationService.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Subscription.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/Log.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <boost/unordered/unordered_flat_set.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  struct LibraryMutationService::MaintenanceGuard::LifetimeState final
  {
    explicit LifetimeState(LibraryMutationService* ownerValue) noexcept
      : _owner{ownerValue}
    {
    }

    template<typename Operation>
    void invokeIfAlive(Operation&& operation)
    {
      auto* liveOwner = static_cast<LibraryMutationService*>(nullptr);

      {
        auto const lock = std::scoped_lock{_mutex};

        if (_owner == nullptr)
        {
          return;
        }

        liveOwner = _owner;
        ++_activeCalls;
      }

      try
      {
        std::invoke(std::forward<Operation>(operation), *liveOwner);
      }
      catch (...)
      {
        releaseCall();
        throw;
      }

      releaseCall();
    }

    void retire() noexcept
    {
      auto lock = std::unique_lock{_mutex};
      _owner = nullptr;
      _callsCompleted.wait(lock, [this] { return _activeCalls == 0; });
    }

  private:
    void releaseCall() noexcept
    {
      bool allCallsCompleted = false;
      {
        auto const lock = std::scoped_lock{_mutex};
        --_activeCalls;
        allCallsCompleted = _activeCalls == 0;
      }

      if (allCallsCompleted)
      {
        _callsCompleted.notify_all();
      }
    }

    std::mutex _mutex;
    std::condition_variable _callsCompleted;
    LibraryMutationService* _owner;
    std::size_t _activeCalls = 0;
  };

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

    [[noreturn]] void terminateLibraryInfrastructure(std::string_view const context,
                                                     std::exception_ptr const exceptionPtr = {}) noexcept
    {
      try
      {
        if (exceptionPtr)
        {
          try
          {
            std::rethrow_exception(exceptionPtr);
          }
          catch (std::exception const& error)
          {
            APP_LOG_CRITICAL("{}: {}", context, error.what());
          }
          catch (...)
          {
            APP_LOG_CRITICAL("{}: unknown exception", context);
          }
        }
        else
        {
          APP_LOG_CRITICAL("{}", context);
        }
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): Logging cannot weaken the terminal infrastructure boundary.
      catch (...)
      {
        // Termination is the final live-runtime infrastructure boundary.
      }

      std::terminate();
    }
  } // namespace

  LibraryMutationService::Mutation::Mutation(LibraryMutationService& owner,
                                             std::unique_lock<std::mutex> writerLock,
                                             library::WriteTransaction transaction)
    : _owner{&owner}, _writerLock{std::move(writerLock)}, _transaction{std::move(transaction)}
  {
  }

  LibraryMutationService::Mutation::~Mutation()
  {
    abort();
  }

  LibraryMutationService::Mutation::Mutation(Mutation&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}
    , _writerLock{std::move(other._writerLock)}
    , _transaction{std::move(other._transaction)}
    , _terminal{std::exchange(other._terminal, true)}
  {
  }

  void LibraryMutationService::Mutation::abort() noexcept
  {
    if (_terminal)
    {
      return;
    }

    _terminal = true;
    _transaction.abort();

    if (_writerLock.owns_lock())
    {
      _writerLock.unlock();
    }
  }

  Result<LibraryMutationService::CommitInfo> LibraryMutationService::Mutation::commit(LibraryChangeSet changeSet)
  {
    if (_owner == nullptr || _terminal)
    {
      return makeError(Error::Code::InvalidState, "Library mutation is already terminal");
    }

    try
    {
      return _owner->commit(*this, std::move(changeSet));
    }
    catch (...)
    {
      abort();
      throw;
    }
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(std::weak_ptr<LifetimeState> lifetimeStatePtr,
                                                             std::uint64_t generation) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _generation{generation}
  {
  }

  LibraryMutationService::MaintenanceGuard::~MaintenanceGuard()
  {
    auto const generation = std::exchange(_generation, 0);

    if (generation == 0)
    {
      return;
    }

    if (auto const lifetimeStatePtr = _lifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
    {
      lifetimeStatePtr->invokeIfAlive([generation](LibraryMutationService& owner) noexcept
                                      { owner.dispatchMaintenanceFinish(generation); });
    }
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(MaintenanceGuard&& other) noexcept
    : _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  void LibraryMutationService::MaintenanceGuard::finish() noexcept
  {
    auto const generation = std::exchange(_generation, 0);

    if (generation == 0)
    {
      return;
    }

    if (auto const lifetimeStatePtr = _lifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
    {
      lifetimeStatePtr->invokeIfAlive([generation](LibraryMutationService& owner) noexcept
                                      { owner.dispatchMaintenanceFinish(generation); });
    }
  }

  LibraryMutationService::LibraryMutationService(async::Executor& callbackExecutor,
                                                 library::WritableMusicLibrary writableLibrary,
                                                 LibraryChanges& changes)
    : _callbackExecutor{callbackExecutor}
    , _writableLibrary{std::move(writableLibrary)}
    , _library{_writableLibrary.library()}
    , _changes{changes}
    , _runtimeInstanceId{nextRuntimeInstanceId()}
    , _lifetimeStatePtr{std::make_shared<MaintenanceGuard::LifetimeState>(this)}
    , _lastCommittedRevision{currentLibraryRevision(_library)}
    , _availableRevision{_lastCommittedRevision}
  {
  }

  LibraryMutationService::~LibraryMutationService()
  {
    _lifetimeStatePtr->retire();
  }

  LibraryAuthoringAvailability LibraryMutationService::availability() const
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return availabilityLocked();
  }

  async::Subscription LibraryMutationService::onAvailabilityChanged(
    std::move_only_function<void(LibraryAuthoringAvailability const&) noexcept> handler) const
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

  Result<BoundListOrder> LibraryMutationService::bindListOrder(ListId const listId,
                                                               std::span<TrackId const> const effectiveTrackIds) const
  {
    return bindListOrder(listId, std::vector<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()});
  }

  Result<BoundListOrder> LibraryMutationService::bindListOrder(ListId const listId,
                                                               std::vector<TrackId>&& effectiveTrackIds) const
  {
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

    if (isVirtualListId(listId))
    {
      return makeError(Error::Code::InvalidInput, "Manual order requires a saved List");
    }

    if (!_library.lists().reader(transaction).get(listId))
    {
      return makeError(Error::Code::NotFound, std::format("List order target not found: {}", listId));
    }

    auto remainingTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
    remainingTrackIds.reserve(effectiveTrackIds.size());

    for (auto const trackId : effectiveTrackIds)
    {
      if (trackId == kInvalidTrackId || !remainingTrackIds.insert(trackId).second)
      {
        return makeError(Error::Code::InvalidInput, std::format("Invalid effective List order track: {}", trackId));
      }
    }

    auto trackReader = _library.tracks().reader(transaction);
    auto const rowCount = trackReader.entryCount(library::TrackStore::Reader::LoadMode::Hot);
    constexpr std::size_t kCursorScanDensityDenominator = 4;
    auto const minimumDenseSelection = (rowCount / kCursorScanDensityDenominator) +
                                       static_cast<std::size_t>(rowCount % kCursorScanDensityDenominator != 0);
    auto const useCursorScan = rowCount != 0 && effectiveTrackIds.size() >= minimumDenseSelection;

    if (useCursorScan)
    {
      for (auto&& [storedTrackId, view] : trackReader.hot())
      {
        std::ignore = view;
        remainingTrackIds.erase(storedTrackId);

        if (remainingTrackIds.empty())
        {
          break;
        }
      }
    }
    else
    {
      for (auto const trackId : effectiveTrackIds)
      {
        if (trackReader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
        {
          remainingTrackIds.erase(trackId);
        }
      }
    }

    if (!remainingTrackIds.empty())
    {
      auto const missingTrackId = *std::ranges::find_if(
        effectiveTrackIds, [&remainingTrackIds](TrackId const trackId) { return remainingTrackIds.contains(trackId); });
      return makeError(
        Error::Code::InvalidInput, std::format("Invalid effective List order track: {}", missingTrackId));
    }

    return BoundListOrder{runtimeInstanceId, revision, listId, std::move(effectiveTrackIds)};
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

        if (_closing)
        {
          return makeError(Error::Code::InvalidState, std::format("{} is unavailable while closing", operation));
        }

        if (writerAdmissionBlockedLocked())
        {
          if (_callbackExecutor.isCurrent())
          {
            return makeError(
              Error::Code::InvalidState,
              std::format("{} cannot start reentrantly during library publication or notification", operation));
          }

          _writerAdmissionChanged.wait(stateLock, [this] { return !writerAdmissionBlockedLocked() || _closing; });

          if (_closing)
          {
            return makeError(Error::Code::InvalidState, std::format("{} is unavailable while closing", operation));
          }
        }

        if (_state != requiredState)
        {
          return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
        }
      }

      auto writerLock = std::unique_lock{_writerMutex};
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing)
      {
        return makeError(Error::Code::InvalidState, std::format("{} is unavailable while closing", operation));
      }

      if (writerAdmissionBlockedLocked())
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

    for (auto const trackId : targets._trackIds)
    {
      gsl_Assert(trackId != kInvalidTrackId);
      gsl_Assert(reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot));
    }

    auto result = AuthoringStart{.status = TrackAuthoringStatus::NoOp};
    result.optMutation.emplace(Mutation{*this, std::move(*writerLockResult), std::move(transaction)});
    return result;
  }

  LibraryMutationService::ListOrderAuthoringStart LibraryMutationService::beginListOrderAuthoringMutation(
    BoundListOrder const& order)
  {
    if (order._runtimeInstanceId != _runtimeInstanceId)
    {
      return ListOrderAuthoringStart{.status = ListOrderAuthoringStatus::Stale};
    }

    auto writerLockResult = acquireWriter(LibraryAuthoringState::Available, "List order authoring");

    if (!writerLockResult)
    {
      return ListOrderAuthoringStart{.status = ListOrderAuthoringStatus::Unavailable};
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (order._runtimeInstanceId != _runtimeInstanceId || order._libraryRevision != _availableRevision)
      {
        return ListOrderAuthoringStart{.status = ListOrderAuthoringStatus::Stale};
      }
    }

    auto transaction = _writableLibrary.writeTransaction();
    gsl_Assert(order._listId != kInvalidListId);
    gsl_Assert(_library.lists().writer(transaction).get(order._listId));
    auto result = ListOrderAuthoringStart{.status = ListOrderAuthoringStatus::NoOp};
    result.optMutation.emplace(Mutation{*this, std::move(*writerLockResult), std::move(transaction)});
    return result;
  }

  Result<LibraryMutationService::MaintenanceGuard> LibraryMutationService::beginMaintenance(LibraryMaintenanceKind kind)
  {
    if (kind == LibraryMaintenanceKind::None)
    {
      return makeError(Error::Code::InvalidInput, "Library maintenance requires an operation kind");
    }

    if (!_callbackExecutor.isCurrent())
    {
      return makeError(Error::Code::InvalidState, "Library maintenance must begin on the callback executor");
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

    emitAvailability(expected, *writerLockResult);
    writerLockResult->unlock();

    return MaintenanceGuard{_lifetimeStatePtr, generation};
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

      if (guard._lifetimeStatePtr.lock() != _lifetimeStatePtr || guard._generation != _maintenanceGeneration)
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

    auto finishTransaction = [&mutation]
    {
      mutation._terminal = true;
      // Store writers borrow the transaction wrapper and may remain in scope
      // until their owning operation returns. Finish the native transaction,
      // but retain its wrapper so those writers can observe the terminal state.
      mutation._transaction.abort();
    };
    auto releaseMutation = [&mutation, &finishTransaction]
    {
      finishTransaction();
      mutation._writerLock.unlock();
    };

    auto const revision = _library.libraryRevision(mutation._transaction);

    std::uint64_t expectedRevision = 0;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      expectedRevision = _lastCommittedRevision + 1U;
    }

    if (revision != expectedRevision)
    {
      releaseMutation();
      terminateLibraryInfrastructure(
        std::format("Library revision gap before commit: expected {}, got {}", expectedRevision, revision));
    }

    auto commitResult = Result<>{};

    try
    {
      commitResult = mutation._transaction.commit();
    }
    catch (...)
    {
      releaseMutation();
      throw;
    }

    if (!commitResult)
    {
      releaseMutation();
      return std::unexpected{commitResult.error()};
    }

    auto const submissionFromOwner = _callbackExecutor.isCurrent();

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      _lastCommittedRevision = revision;
      gsl_Assert(!_publicationBarrier.blocksWriter());
      _publicationBarrier.beginSubmission(submissionFromOwner);
    }

    // Logical publication gates keep writer and Closing admission closed while
    // the physical writer mutex is released before external delivery. The
    // admission gate remains set until P has completed inline or the callback
    // executor has accepted it.
    finishTransaction();
    changeSet.libraryRevision = revision;
    mutation._writerLock.unlock();
    gsl_Assert(!mutation._writerLock.owns_lock());

    try
    {
      _changes.publishFromCoordinator(
        std::move(changeSet),
        [weakLifetimeStatePtr = std::weak_ptr<MaintenanceGuard::LifetimeState>{_lifetimeStatePtr}, revision] noexcept
        {
          if (auto const lifetimeStatePtr = weakLifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
          {
            lifetimeStatePtr->invokeIfAlive([revision](LibraryMutationService& owner) noexcept
                                            { owner.finishPublication(revision); });
          }
        });
    }
    catch (...)
    {
      terminateLibraryInfrastructure(
        "Committed library revision could not enter mandatory publication", std::current_exception());
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      gsl_Assert(_publicationBarrier.submissionInProgress());
      _publicationBarrier.completeSubmission();
    }

    _writerAdmissionChanged.notify_all();
    return CommitInfo{.libraryRevision = revision};
  }

  void LibraryMutationService::beginClosing() noexcept
  {
    while (true)
    {
      auto writerLock = std::unique_lock{_writerMutex};
      auto stateLock = std::unique_lock{_stateMutex};

      if (_closing)
      {
        return;
      }

      if (_publicationBarrier.submissionInProgress() || _availabilityNotificationInProgress)
      {
        if (_publicationBarrier.ownerSubmissionInProgress() || _availabilityNotificationInProgress)
        {
          // Destroying an owner-thread publisher or availability emitter from
          // its synchronous observer would resume through a dead object.
          gsl_Expects(!_callbackExecutor.isCurrent());
        }

        writerLock.unlock();
        _writerAdmissionChanged.wait(
          stateLock,
          [this]
          {
            return (!_publicationBarrier.submissionInProgress() && !_availabilityNotificationInProgress) || _closing;
          });
        continue;
      }

      _closing = true;
      gsl_Assert(!_publicationBarrier.submissionInProgress());
      _publicationBarrier.retire();
      _maintenanceKind = LibraryMaintenanceKind::None;
      _changes.retireFromCoordinator();
      break;
    }

    _writerAdmissionChanged.notify_all();
    _lifetimeStatePtr->retire();
  }

  void LibraryMutationService::dispatchMaintenanceFinish(std::uint64_t const generation) noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing)
      {
        return;
      }
    }

    if (_callbackExecutor.isCurrent())
    {
      finishMaintenance(generation);
      return;
    }

    try
    {
      _callbackExecutor.dispatch(
        [weakLifetimeStatePtr = std::weak_ptr<MaintenanceGuard::LifetimeState>{_lifetimeStatePtr}, generation] noexcept
        {
          if (auto const lifetimeStatePtr = weakLifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
          {
            lifetimeStatePtr->invokeIfAlive([generation](LibraryMutationService& owner) noexcept
                                            { owner.finishMaintenance(generation); });
          }
        });
    }
    catch (...)
    {
      {
        auto const stateLock = std::scoped_lock{_stateMutex};

        if (_closing)
        {
          return;
        }
      }

      terminateLibraryInfrastructure(
        "Live library maintenance could not return to the callback executor", std::current_exception());
    }
  }

  void LibraryMutationService::handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing)
      {
        return;
      }
    }

    terminateLibraryInfrastructure(
      "Live library maintenance finalization was rejected by the callback executor", std::move(exceptionPtr));
  }

  void LibraryMutationService::finishMaintenance(std::uint64_t const generation) noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing || _state != LibraryAuthoringState::Maintenance || generation != _maintenanceGeneration)
      {
        return;
      }
    }

    auto writerLockResult = acquireWriter(LibraryAuthoringState::Maintenance, "Library maintenance completion");

    if (!writerLockResult)
    {
      {
        auto const stateLock = std::scoped_lock{_stateMutex};

        if (_closing)
        {
          return;
        }
      }

      terminateLibraryInfrastructure("Library maintenance completion violated publication ordering");
    }

    auto expected = LibraryAuthoringAvailability{};

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing || _state != LibraryAuthoringState::Maintenance || generation != _maintenanceGeneration)
      {
        return;
      }

      _state = LibraryAuthoringState::Available;
      _maintenanceKind = LibraryMaintenanceKind::None;
      expected = availabilityLocked();
    }

    emitAvailability(expected, *writerLockResult);
    writerLockResult->unlock();
  }

  void LibraryMutationService::finishPublication(std::uint64_t const revision) noexcept
  {
    auto expected = LibraryAuthoringAvailability{};
    bool shouldEmit = false;
    bool invariantViolated = false;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing)
      {
        return;
      }

      if (!_publicationBarrier.publicationInProgress() || revision != _lastCommittedRevision)
      {
        invariantViolated = true;
      }
      else
      {
        _availableRevision = revision;
        shouldEmit = _state == LibraryAuthoringState::Available;
        expected = availabilityLocked();
      }
    }

    if (invariantViolated)
    {
      terminateLibraryInfrastructure("Library publication completion did not match the committed revision");
    }

    if (shouldEmit)
    {
      emitAvailability(expected);
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_closing)
      {
        return;
      }

      _publicationBarrier.completePublication();
    }

    _writerAdmissionChanged.notify_all();
  }

  bool LibraryMutationService::beginAvailabilityNotification(LibraryAuthoringAvailability const& expected) noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};

    if (_closing || availabilityLocked() != expected)
    {
      return false;
    }

    gsl_Assert(!_availabilityNotificationInProgress);
    _availabilityNotificationInProgress = true;
    return true;
  }

  void LibraryMutationService::completeAvailabilityNotification() noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      gsl_Assert(_availabilityNotificationInProgress);
      _availabilityNotificationInProgress = false;
    }

    _writerAdmissionChanged.notify_all();
  }

  void LibraryMutationService::emitAvailability(LibraryAuthoringAvailability const& expected) noexcept
  {
    if (!beginAvailabilityNotification(expected))
    {
      return;
    }

    _availabilityChanged.emit(expected);
    completeAvailabilityNotification();
  }

  void LibraryMutationService::emitAvailability(LibraryAuthoringAvailability const& expected,
                                                std::unique_lock<std::mutex>& writerLock) noexcept
  {
    gsl_Expects(writerLock.owns_lock());

    if (!beginAvailabilityNotification(expected))
    {
      return;
    }

    writerLock.unlock();
    _availabilityChanged.emit(expected);
    writerLock.lock();
    completeAvailabilityNotification();
  }

  bool LibraryMutationService::writerAdmissionBlockedLocked() const noexcept
  {
    return _publicationBarrier.blocksWriter() || _availabilityNotificationInProgress;
  }

  LibraryAuthoringAvailability LibraryMutationService::availabilityLocked() const noexcept
  {
    return LibraryAuthoringAvailability{.state = _state,
                                        .runtimeInstanceId = _runtimeInstanceId,
                                        .libraryRevision = _availableRevision,
                                        .maintenanceKind = _state == LibraryAuthoringState::Maintenance
                                                             ? _maintenanceKind
                                                             : LibraryMaintenanceKind::None};
  }
} // namespace ao::rt
