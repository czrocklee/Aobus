// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryMutationService.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  void detail::requireMatchingPublicationCompletion(bool const publicationInProgress,
                                                    std::uint64_t const revision,
                                                    std::uint64_t const committedRevision,
                                                    std::string_view const libraryIdentity,
                                                    std::string_view const replicaName)
  {
    if (!publicationInProgress || revision != committedRevision)
    {
      AO_FATAL("library publication failure: phase=completion-ack library='{}' revision={} replica='{}' "
               "committed-revision={} publication-active={}",
               libraryIdentity,
               revision,
               replicaName,
               committedRevision,
               publicationInProgress);
    }
  }

  namespace
  {
    template<typename Value>
    class OneShotEvent final
    {
    public:
      async::Task<Value> wait() const
      {
        auto statePtr = _statePtr;
        return boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(Value)>(
          [statePtr](auto handler) mutable
          {
            auto executor = boost::asio::get_associated_executor(handler);
            auto completion = compat::MoveOnlyFunction<void(Value)>{
              [executor, handler = std::move(handler)](Value value) mutable
              {
                boost::asio::post(executor,
                                  [handler = std::move(handler), value = std::move(value)] mutable
                                  { std::move(handler)(std::move(value)); });
              }};
            auto optReady = std::optional<Value>{};

            {
              auto const lock = std::scoped_lock{statePtr->mutex};
              AO_INVARIANT(!statePtr->completion, "One-shot event has multiple waiters");

              if (!statePtr->optTerminal)
              {
                statePtr->completion = std::move(completion);
                return;
              }

              optReady = statePtr->optTerminal;
            }

            AO_INVARIANT(optReady);
            completion(std::move(*optReady));
          },
          boost::asio::use_awaitable);
      }

      void signal(Value value) noexcept
      {
        auto completion = compat::MoveOnlyFunction<void(Value)>{};

        {
          auto const lock = std::scoped_lock{_statePtr->mutex};
          AO_INVARIANT(!_statePtr->optTerminal, "One-shot event was signalled twice");
          _statePtr->optTerminal = value;
          completion = std::move(_statePtr->completion);
        }

        if (completion)
        {
          try
          {
            completion(std::move(value));
          }
          catch (...)
          {
            AO_FATAL_EXCEPTION(std::current_exception(), "one-shot library sequencer event completion");
          }
        }
      }

    private:
      struct State final
      {
        std::mutex mutex;
        std::optional<Value> optTerminal;
        compat::MoveOnlyFunction<void(Value)> completion;
      };

      std::shared_ptr<State> _statePtr = std::make_shared<State>();
    };

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

    [[noreturn]] void abortLibraryInfrastructure(
      std::string_view const context,
      std::exception_ptr const exceptionPtr = {},
      std::source_location const location = std::source_location::current()) noexcept
    {
      if (exceptionPtr)
      {
        fatalFromException(exceptionPtr, context, location);
      }

      AO_FATAL_AT(location, context);
    }
  } // namespace

  class detail::LibraryMutationOwnerLease final
  {
  public:
    ~LibraryMutationOwnerLease();

    LibraryMutationOwnerLease(LibraryMutationOwnerLease const&) = delete;
    LibraryMutationOwnerLease& operator=(LibraryMutationOwnerLease const&) = delete;
    LibraryMutationOwnerLease(LibraryMutationOwnerLease&&) = delete;
    LibraryMutationOwnerLease& operator=(LibraryMutationOwnerLease&&) = delete;

    LibraryMutationService& owner() const noexcept { return *_owner; }

  private:
    LibraryMutationOwnerLease(std::shared_ptr<LibraryMutationLifetimeState> statePtr,
                              LibraryMutationService& owner) noexcept
      : _statePtr{std::move(statePtr)}, _owner{&owner}
    {
    }

    std::shared_ptr<LibraryMutationLifetimeState> _statePtr;
    LibraryMutationService* _owner;

    friend class LibraryMutationLifetimeState;
  };

  class detail::LibraryMutationLifetimeState final : public std::enable_shared_from_this<LibraryMutationLifetimeState>
  {
  public:
    explicit LibraryMutationLifetimeState(LibraryMutationService* ownerValue) noexcept
      : _owner{ownerValue}
    {
    }

    std::unique_ptr<LibraryMutationOwnerLease> acquire()
    {
      auto const lock = std::scoped_lock{_mutex};

      if (_owner == nullptr)
      {
        return {};
      }

      ++_activeCalls;
      return std::unique_ptr<LibraryMutationOwnerLease>{new LibraryMutationOwnerLease{shared_from_this(), *_owner}};
    }

    template<typename Operation>
    void invokeIfAlive(Operation&& operation)
    {
      auto ownerLeasePtr = acquire();

      if (ownerLeasePtr != nullptr)
      {
        std::invoke(std::forward<Operation>(operation), ownerLeasePtr->owner());
      }
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
        AO_INVARIANT(_activeCalls != 0);
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

    friend class LibraryMutationOwnerLease;
  };

  namespace detail
  {
    LibraryMutationOwnerLease::~LibraryMutationOwnerLease()
    {
      _statePtr->releaseCall();
    }
  } // namespace detail

  struct detail::LibraryMutationCommandRequest final
  {
    explicit LibraryMutationCommandRequest(LibraryMutationService::CommandKind const commandKind)
      : kind{commandKind}
    {
    }

    async::Task<bool> wait() const { return event.wait(); }
    void grant() noexcept { event.signal(true); }
    void retire() noexcept { event.signal(false); }
    std::stop_token closingStopToken() const noexcept { return closingStopSource.get_token(); }
    void requestClosingStop() const noexcept { std::ignore = closingStopSource.request_stop(); }

    LibraryMutationService::CommandKind kind;
    OneShotEvent<bool> event;
    std::stop_source closingStopSource;
  };

  struct detail::LibraryMutationPublicationEvent final
  {
    explicit LibraryMutationPublicationEvent(std::uint64_t const committedRevision)
      : revision{committedRevision}
    {
    }

    async::Task<LibraryPublicationTerminal> wait() const { return event.wait(); }
    void signal(LibraryPublicationTerminal const terminal) noexcept { event.signal(terminal); }

    std::uint64_t revision;
    OneShotEvent<LibraryPublicationTerminal> event;
  };

  struct detail::LibraryMutationControlDelivery final
  {
    explicit LibraryMutationControlDelivery(LibraryAuthoringAvailability availability)
      : expected{availability}
    {
    }

    async::Task<bool> wait() const { return event.wait(); }
    void complete() noexcept { event.signal(true); }
    void retire() noexcept { event.signal(false); }

    LibraryAuthoringAvailability expected;
    OneShotEvent<bool> event;
    bool claimed = false;
    bool retired = false;
  };

  class detail::LibraryMutationCommandLease final
  {
  public:
    LibraryMutationCommandLease(LibraryMutationService& owner,
                                std::shared_ptr<LibraryMutationCommandRequest> requestPtr,
                                std::unique_ptr<LibraryMutationOwnerLease> ownerLeasePtr) noexcept
      : _owner{&owner}, _requestPtr{std::move(requestPtr)}, _ownerLeasePtr{std::move(ownerLeasePtr)}
    {
    }

    ~LibraryMutationCommandLease() { release(); }

    LibraryMutationCommandLease(LibraryMutationCommandLease const&) = delete;
    LibraryMutationCommandLease& operator=(LibraryMutationCommandLease const&) = delete;
    LibraryMutationCommandLease(LibraryMutationCommandLease&&) = delete;
    LibraryMutationCommandLease& operator=(LibraryMutationCommandLease&&) = delete;

    LibraryMutationService& owner() const noexcept { return *_owner; }
    std::stop_token closingStopToken() const noexcept { return _requestPtr->closingStopToken(); }

    void release() noexcept
    {
      if (_owner == nullptr)
      {
        return;
      }

      auto* const owner = std::exchange(_owner, nullptr);
      owner->releaseCommand(_requestPtr);
      _ownerLeasePtr.reset();
    }

  private:
    LibraryMutationService* _owner;
    std::shared_ptr<LibraryMutationCommandRequest> _requestPtr;
    std::unique_ptr<LibraryMutationOwnerLease> _ownerLeasePtr;
  };

  LibraryMutationService::Submission::Submission(std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
                                                 bool const reentrant) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _reentrant{reentrant}
  {
  }

  LibraryMutationService::Mutation::Mutation(LibraryMutationService& owner,
                                             std::unique_ptr<detail::LibraryMutationCommandLease> commandLeasePtr,
                                             library::WriteTransaction transaction) noexcept
    : _owner{&owner}, _commandLeasePtr{std::move(commandLeasePtr)}, _transaction{std::move(transaction)}
  {
  }

  LibraryMutationService::Mutation::~Mutation()
  {
    abort();
  }

  LibraryMutationService::Mutation::Mutation(Mutation&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}
    , _commandLeasePtr{std::move(other._commandLeasePtr)}
    , _transaction{std::move(other._transaction)}
    , _terminal{std::exchange(other._terminal, true)}
    , _executeEligible{std::exchange(other._executeEligible, false)}
  {
  }

  void LibraryMutationService::Mutation::finish() noexcept
  {
    _terminal = true;
    _commandLeasePtr.reset();
  }

  std::stop_token LibraryMutationService::Mutation::closingStopToken() const noexcept
  {
    AO_INVARIANT(_commandLeasePtr != nullptr, "Terminal library mutation has no Closing stop token");
    return _commandLeasePtr->closingStopToken();
  }

  void LibraryMutationService::Mutation::abort() noexcept
  {
    if (!_terminal)
    {
      _terminal = true;
      _transaction.abort();
    }

    _commandLeasePtr.reset();
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation) noexcept
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
      lifetimeStatePtr->invokeIfAlive(
        [generation](LibraryMutationService& owner)
        {
          auto const lock = std::scoped_lock{owner._stateMutex};
          AO_INVARIANT(owner._lifecycle != Lifecycle::Open || generation != owner._maintenanceGeneration,
                       "Live library maintenance guard was destroyed without awaiting finishAsync");
        });
    }
  }

  LibraryMutationService::MaintenanceGuard::MaintenanceGuard(MaintenanceGuard&& other) noexcept
    : _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  async::Task<void> LibraryMutationService::MaintenanceGuard::finishAsync()
  {
    auto const generation = std::exchange(_generation, 0);
    AO_EXPECTS(generation != 0, "Library maintenance guard has already finished");
    return LibraryMutationService::finishMaintenanceAsync(_lifetimeStatePtr, generation);
  }

  LibraryMutationService::BackgroundTaskLease::BackgroundTaskLease(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _generation{generation}
  {
  }

  LibraryMutationService::BackgroundTaskLease::~BackgroundTaskLease()
  {
    finish();
  }

  LibraryMutationService::BackgroundTaskLease::BackgroundTaskLease(BackgroundTaskLease&& other) noexcept
    : _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  void LibraryMutationService::BackgroundTaskLease::finish() noexcept
  {
    auto const generation = std::exchange(_generation, 0);

    if (generation == 0)
    {
      return;
    }

    if (auto const lifetimeStatePtr = _lifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
    {
      lifetimeStatePtr->invokeIfAlive([generation](LibraryMutationService& owner) noexcept
                                      { owner.finishBackgroundTask(generation); });
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
    , _lifetimeStatePtr{std::make_shared<detail::LibraryMutationLifetimeState>(this)}
    , _lastCommittedRevision{currentLibraryRevision(_library)}
    , _availableRevision{_lastCommittedRevision}
  {
  }

  LibraryMutationService::~LibraryMutationService()
  {
    beginClosing();
  }

  LibraryAuthoringAvailability LibraryMutationService::availability() const
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return availabilityLocked();
  }

  async::Subscription LibraryMutationService::onAvailabilityChanged(
    compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const
  {
    return _availabilityChanged.connect(
      [this, handler = std::move(handler)](LibraryAuthoringAvailability const& availability) mutable noexcept
      {
        try
        {
          handler(availability);
        }
        catch (...)
        {
          auto const optDiagnosticContext = activePublicationDiagnosticContext();

          if (optDiagnosticContext)
          {
            AO_FATAL_EXCEPTION(
              std::current_exception(),
              std::format("library publication failure: phase=completion library='{}' revision={} replica='{}'",
                          optDiagnosticContext->libraryIdentity,
                          optDiagnosticContext->revision,
                          optDiagnosticContext->replicaName));
          }

          AO_FATAL_EXCEPTION(std::current_exception(), "library availability observer");
        }
      });
  }

  Result<BoundTrackTargets> LibraryMutationService::bindTrackTargets(std::span<TrackId const> const trackIds) const
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

      if (_lifecycle != Lifecycle::Open || _state != LibraryAuthoringState::Available || revision != _availableRevision)
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

      if (_lifecycle != Lifecycle::Open || _state != LibraryAuthoringState::Available || revision != _availableRevision)
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
    auto const rowCount = trackReader.entryCount();
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
                                                                std::uint64_t const revision)
  {
    return BoundTrackTargets{
      targets._runtimeInstanceId, revision, std::vector<TrackId>{targets._trackIds.begin(), targets._trackIds.end()}};
  }

  LibraryMutationService::Submission LibraryMutationService::captureSubmission() const noexcept
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return Submission{_lifetimeStatePtr, _callbackExecutor.isCurrent() && submissionIsReentrantLocked()};
  }

  Result<std::shared_ptr<detail::LibraryMutationCommandRequest>> LibraryMutationService::enqueueCommand(
    CommandKind const kind,
    std::uint64_t const generation,
    bool const reentrant,
    std::string_view const operation)
  {
    auto requestPtr = std::make_shared<detail::LibraryMutationCommandRequest>(kind);
    bool grantNow = false;

    {
      auto const lock = std::scoped_lock{_stateMutex};

      if (_lifecycle != Lifecycle::Open)
      {
        return makeError(Error::Code::InvalidState, std::format("{} is unavailable while closing", operation));
      }

      if (reentrant)
      {
        return makeError(
          Error::Code::InvalidState,
          std::format("{} cannot start reentrantly during library publication or notification", operation));
      }

      switch (kind)
      {
        case CommandKind::Interactive:
          if (_state != LibraryAuthoringState::Available || hasMaintenanceTransitionLocked())
          {
            return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
          }

          if (hasOutstandingCommandLocked(CommandKind::Interactive) ||
              hasOutstandingCommandLocked(CommandKind::Background))
          {
            return makeError(Error::Code::ResourceBusy, std::format("{} is busy", operation));
          }

          break;
        case CommandKind::Background:
          if (_state != LibraryAuthoringState::Available || hasMaintenanceTransitionLocked() ||
              generation != _backgroundTaskGeneration || !_optBackgroundTaskKind)
          {
            return makeError(Error::Code::InvalidState, "Library background task is no longer active");
          }

          if (hasOutstandingCommandLocked(CommandKind::Background))
          {
            return makeError(Error::Code::ResourceBusy, "Another library background mutation is already active");
          }

          break;
        case CommandKind::MaintenanceMutation:
          if (_state != LibraryAuthoringState::Maintenance || generation != _maintenanceGeneration)
          {
            return makeError(Error::Code::InvalidState, "Library maintenance session is no longer active");
          }

          break;
        case CommandKind::MaintenanceEnter:
          if (_state != LibraryAuthoringState::Available || hasMaintenanceTransitionLocked())
          {
            return makeError(Error::Code::InvalidState, "Library maintenance is unavailable");
          }

          if (_optBackgroundTaskKind && *_optBackgroundTaskKind != BackgroundTaskKind::Import)
          {
            return makeError(Error::Code::InvalidState, "Library maintenance conflicts with an active background task");
          }

          break;
        case CommandKind::MaintenanceExit:
          if (_state != LibraryAuthoringState::Maintenance || generation != _maintenanceGeneration ||
              hasMaintenanceTransitionLocked())
          {
            return makeError(Error::Code::InvalidState, "Library maintenance session is no longer active");
          }

          break;
      }

      if (_activePhase == ActivePhase::Idle)
      {
        _activePhase = ActivePhase::PreTransaction;
        _activeCommandRequestPtr = requestPtr;
        grantNow = true;
      }
      else
      {
        _commandQueue.push_back(requestPtr);
      }
    }

    if (grantNow)
    {
      requestPtr->grant();
    }

    return requestPtr;
  }

  async::Task<Result<std::unique_ptr<detail::LibraryMutationCommandLease>>> LibraryMutationService::acquireCommandAsync(
    Submission submission,
    CommandKind const kind,
    std::uint64_t const generation,
    std::string operation)
  {
    auto lifetimeStatePtr = submission._lifetimeStatePtr.lock();

    if (lifetimeStatePtr == nullptr)
    {
      co_return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
    }

    auto ownerLeasePtr = lifetimeStatePtr->acquire();

    if (ownerLeasePtr == nullptr)
    {
      co_return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
    }

    auto& owner = ownerLeasePtr->owner();
    auto requestRes = owner.enqueueCommand(kind, generation, submission._reentrant, operation);

    if (!requestRes)
    {
      co_return std::unexpected{requestRes.error()};
    }

    if (!co_await (*requestRes)->wait())
    {
      async::throwOperationCancelled();
    }

    auto commandLeasePtr =
      std::make_unique<detail::LibraryMutationCommandLease>(owner, std::move(*requestRes), std::move(ownerLeasePtr));
    bool closing = false;

    {
      auto const lock = std::scoped_lock{owner._stateMutex};
      closing = owner._lifecycle != Lifecycle::Open;
    }

    if (closing)
    {
      commandLeasePtr.reset();
      async::throwOperationCancelled();
    }

    co_return commandLeasePtr;
  }

  async::Task<Result<LibraryMutationService::Mutation>> LibraryMutationService::beginMutationAsync(
    Submission submission,
    CommandKind const kind,
    std::uint64_t const generation,
    library::WriteTransaction::Options options,
    std::string operation,
    compat::MoveOnlyFunction<Result<>(std::stop_token)> preTransaction)
  {
    auto commandLeaseRes = co_await acquireCommandAsync(std::move(submission), kind, generation, operation);

    if (!commandLeaseRes)
    {
      co_return std::unexpected{commandLeaseRes.error()};
    }

    auto commandLeasePtr = std::move(*commandLeaseRes);
    auto& owner = commandLeasePtr->owner();

    if (preTransaction)
    {
      auto preTransactionRes = preTransaction(commandLeasePtr->closingStopToken());

      if (!preTransactionRes)
      {
        commandLeasePtr.reset();
        co_return std::unexpected{preTransactionRes.error()};
      }
    }

    if (!owner.beginTransaction())
    {
      commandLeasePtr.reset();
      async::throwOperationCancelled();
    }

    co_return Mutation{owner, std::move(commandLeasePtr), owner._writableLibrary.writeTransaction(std::move(options))};
  }

  async::Task<Result<LibraryMutationService::Mutation>> LibraryMutationService::beginInteractiveMutationAsync(
    Submission submission,
    library::WriteTransaction::Options options)
  {
    return beginMutationAsync(
      std::move(submission), CommandKind::Interactive, 0, std::move(options), "Library mutation");
  }

  async::Task<LibraryMutationService::AuthoringStart> LibraryMutationService::beginAuthoringMutationAsync(
    Submission submission,
    BoundTrackTargets targets)
  {
    auto mutationRes =
      co_await beginMutationAsync(std::move(submission), CommandKind::Interactive, 0, {}, "Track authoring");

    if (!mutationRes)
    {
      auto const status =
        mutationRes.error().code == Error::Code::ResourceBusy ? AuthoringStatus::Busy : AuthoringStatus::Unavailable;
      co_return AuthoringStart{.status = status};
    }

    auto mutation = std::move(*mutationRes);
    auto& owner = *mutation._owner;
    bool stale = false;

    {
      auto const stateLock = std::scoped_lock{owner._stateMutex};
      stale = !targets.matches(owner.availabilityLocked());
    }

    if (stale)
    {
      mutation.abort();
      co_return AuthoringStart{.status = AuthoringStatus::Stale};
    }

    auto reader = owner._library.tracks().reader(mutation._transaction);

    for (auto const trackId : targets._trackIds)
    {
      AO_INVARIANT(trackId != kInvalidTrackId);
      AO_INVARIANT(reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot));
    }

    co_return AuthoringStart{.status = AuthoringStatus::NoOp, .optMutation = std::move(mutation)};
  }

  async::Task<LibraryMutationService::AuthoringStart> LibraryMutationService::beginListOrderAuthoringMutationAsync(
    Submission submission,
    BoundListOrder order)
  {
    auto mutationRes =
      co_await beginMutationAsync(std::move(submission), CommandKind::Interactive, 0, {}, "List order authoring");

    if (!mutationRes)
    {
      auto const status =
        mutationRes.error().code == Error::Code::ResourceBusy ? AuthoringStatus::Busy : AuthoringStatus::Unavailable;
      co_return AuthoringStart{.status = status};
    }

    auto mutation = std::move(*mutationRes);
    auto& owner = *mutation._owner;
    bool stale = false;

    {
      auto const stateLock = std::scoped_lock{owner._stateMutex};
      stale = !order.matches(owner.availabilityLocked());
    }

    if (stale)
    {
      mutation.abort();
      co_return AuthoringStart{.status = AuthoringStatus::Stale};
    }

    AO_INVARIANT(order._listId != kInvalidListId);
    AO_INVARIANT(owner._library.lists().reader(mutation._transaction).get(order._listId));
    co_return AuthoringStart{.status = AuthoringStatus::NoOp, .optMutation = std::move(mutation)};
  }

  Result<LibraryMutationService::BackgroundTaskLease> LibraryMutationService::beginBackgroundTask(
    BackgroundTaskKind const kind)
  {
    AO_EXPECTS(_callbackExecutor.isCurrent(), "Library background task must begin on the callback executor");
    auto const stateLock = std::scoped_lock{_stateMutex};

    if (_lifecycle != Lifecycle::Open)
    {
      return makeError(Error::Code::InvalidState, "Library background task is unavailable while closing");
    }

    if (_state != LibraryAuthoringState::Available || hasMaintenanceTransitionLocked())
    {
      return makeError(Error::Code::InvalidState, "Library background task is unavailable during maintenance");
    }

    if (_optBackgroundTaskKind)
    {
      return makeError(Error::Code::ResourceBusy, "Another library background task is already active");
    }

    _optBackgroundTaskKind = kind;
    auto const generation = ++_backgroundTaskGeneration;
    return BackgroundTaskLease{_lifetimeStatePtr, generation};
  }

  async::Task<Result<LibraryMutationService::Mutation>> LibraryMutationService::beginBackgroundMutationAsync(
    Submission submission,
    BackgroundTaskLease const& lease,
    compat::MoveOnlyFunction<Result<>(std::stop_token)> preTransaction)
  {
    auto const lockedLeaseStatePtr = lease._lifetimeStatePtr.lock();
    auto const submissionStatePtr = submission._lifetimeStatePtr.lock();

    if (lockedLeaseStatePtr == nullptr || lockedLeaseStatePtr != submissionStatePtr)
    {
      return async::makeReadyTask(
        Result<Mutation>{makeError(Error::Code::InvalidState, "Library background task is no longer active")});
    }

    return beginMutationAsync(std::move(submission),
                              CommandKind::Background,
                              lease._generation,
                              {},
                              "Library background mutation",
                              std::move(preTransaction));
  }

  bool LibraryMutationService::beginTransaction() noexcept
  {
    auto const lock = std::scoped_lock{_stateMutex};

    if (_lifecycle != Lifecycle::Open)
    {
      return false;
    }

    AO_INVARIANT(_activePhase == ActivePhase::PreTransaction);
    _activePhase = ActivePhase::InTransaction;
    _stateChanged.notify_all();
    return true;
  }

  Result<std::uint64_t> LibraryMutationService::commitMutation(Mutation& mutation, LibraryChangeSet changeSet)
  {
    AO_EXPECTS(mutation._owner == this && mutation._commandLeasePtr != nullptr && !mutation._terminal,
               "Library mutation does not belong to this service");
    AO_INVARIANT(changeSet.libraryRevision == 0, "Library operation produced a pre-stamped change set");

    auto const revision = _library.libraryRevision(mutation._transaction);
    std::uint64_t expectedRevision = 0;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase == ActivePhase::InTransaction);
      expectedRevision = _lastCommittedRevision + 1U;
    }

    if (revision != expectedRevision)
    {
      mutation.abort();
      abortLibraryInfrastructure(
        std::format("Library revision gap before commit: expected {}, got {}", expectedRevision, revision));
    }

    auto publicationEventPtr = std::make_shared<detail::LibraryMutationPublicationEvent>(revision);
    auto publicationCompletion =
      compat::MoveOnlyFunction<void(detail::LibraryPublicationTerminal, std::string, std::string)>{
        [weakLifetimeStatePtr = std::weak_ptr<detail::LibraryMutationLifetimeState>{_lifetimeStatePtr}, revision](
          detail::LibraryPublicationTerminal const terminal, std::string libraryIdentity, std::string replicaName)
        {
          if (auto const lifetimeStatePtr = weakLifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
          {
            lifetimeStatePtr->invokeIfAlive(
              [terminal, revision, libraryIdentity = std::move(libraryIdentity), replicaName = std::move(replicaName)](
                LibraryMutationService& owner) mutable
              { owner.finishPublication(terminal, revision, std::move(libraryIdentity), std::move(replicaName)); });
          }
        }};

    auto commitRes = Result<>{};

    try
    {
      commitRes = mutation._transaction.commit();
    }
    catch (...)
    {
      mutation.abort();
      throw;
    }

    if (!commitRes)
    {
      mutation.abort();
      return std::unexpected{commitRes.error()};
    }

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase == ActivePhase::InTransaction);
      AO_INVARIANT(_activePublicationEventPtr == nullptr);
      _lastCommittedRevision = revision;
      _activePublicationEventPtr = publicationEventPtr;
      _activePhase = ActivePhase::SubmittingPublication;
    }

    mutation._terminal = true;
    mutation._transaction.abort();
    changeSet.libraryRevision = revision;
    _changes.publishFromCoordinator(std::move(changeSet), std::move(publicationCompletion));

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase == ActivePhase::SubmittingPublication);
      _activePhase = ActivePhase::AwaitingPublication;
    }

    _stateChanged.notify_all();
    return revision;
  }

  async::Task<detail::LibraryPublicationTerminal> LibraryMutationService::settleMutationAsync(
    std::uint64_t const revision)
  {
    auto eventPtr = std::shared_ptr<detail::LibraryMutationPublicationEvent>{};

    {
      auto const lock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePublicationEventPtr != nullptr && _activePublicationEventPtr->revision == revision);
      eventPtr = _activePublicationEventPtr;
    }

    auto const terminal = co_await eventPtr->wait();
    completeCommittedCommand(revision);
    co_return terminal;
  }

  [[noreturn]] void LibraryMutationService::abortPostCommitSettlement(std::exception_ptr const exceptionPtr) noexcept
  {
    abortLibraryInfrastructure("Committed library mutation could not reach publication settlement", exceptionPtr);
  }

  void LibraryMutationService::completeCommittedCommand(std::uint64_t const revision) noexcept
  {
    auto const lock = std::scoped_lock{_stateMutex};
    AO_INVARIANT(_activePhase == ActivePhase::AwaitingPublication);
    AO_INVARIANT(_activePublicationEventPtr != nullptr && _activePublicationEventPtr->revision == revision);
    _activePublicationEventPtr.reset();
  }

  void LibraryMutationService::finishPublication(detail::LibraryPublicationTerminal const terminal,
                                                 std::uint64_t const revision,
                                                 std::string libraryIdentity,
                                                 std::string replicaName)
  {
    auto eventPtr = std::shared_ptr<detail::LibraryMutationPublicationEvent>{};
    auto expected = LibraryAuthoringAvailability{};
    std::uint64_t committedRevision = 0;
    bool shouldEmit = false;
    bool publicationInProgress = false;

    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      publicationInProgress = _activePublicationEventPtr != nullptr;
      committedRevision = _lastCommittedRevision;

      if (publicationInProgress && revision == committedRevision)
      {
        eventPtr = _activePublicationEventPtr;

        if (terminal == detail::LibraryPublicationTerminal::Published)
        {
          _availableRevision = revision;
          shouldEmit = _lifecycle == Lifecycle::Open && _state == LibraryAuthoringState::Available;
          expected = availabilityLocked();
        }
      }
    }

    detail::requireMatchingPublicationCompletion(
      publicationInProgress, revision, committedRevision, libraryIdentity, replicaName);

    if (shouldEmit)
    {
      {
        auto const stateLock = std::scoped_lock{_stateMutex};
        AO_INVARIANT(!_optActivePublicationDiagnosticContext);
        _optActivePublicationDiagnosticContext.emplace(PublicationDiagnosticContext{
          .libraryIdentity = std::move(libraryIdentity),
          .replicaName = std::move(replicaName),
          .revision = revision,
        });
      }

      emitAvailability(expected);

      {
        auto const stateLock = std::scoped_lock{_stateMutex};
        AO_INVARIANT(_optActivePublicationDiagnosticContext);
        _optActivePublicationDiagnosticContext.reset();
      }
    }

    eventPtr->signal(terminal);
    _stateChanged.notify_all();
  }

  void LibraryMutationService::releaseCommand(
    std::shared_ptr<detail::LibraryMutationCommandRequest> const& requestPtr) noexcept
  {
    auto nextRequestPtr = std::shared_ptr<detail::LibraryMutationCommandRequest>{};

    {
      auto const lock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase != ActivePhase::Idle);
      AO_INVARIANT(_activeCommandRequestPtr == requestPtr);

      _activePhase = ActivePhase::Idle;
      _activeCommandRequestPtr.reset();

      if (_lifecycle == Lifecycle::Open && !_commandQueue.empty())
      {
        nextRequestPtr = std::move(_commandQueue.front());
        _commandQueue.pop_front();
        _activeCommandRequestPtr = nextRequestPtr;
        _activePhase = ActivePhase::PreTransaction;
      }
    }

    _stateChanged.notify_all();

    if (nextRequestPtr != nullptr)
    {
      nextRequestPtr->grant();
    }
  }

  void LibraryMutationService::finishBackgroundTask(std::uint64_t const generation) noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};

    if (_lifecycle != Lifecycle::Open || generation != _backgroundTaskGeneration)
    {
      return;
    }

    _optBackgroundTaskKind.reset();
  }

  async::Task<Result<LibraryMutationService::MaintenanceGuard>> LibraryMutationService::beginMaintenanceAsync(
    Submission submission)
  {
    auto commandLeaseRes =
      co_await acquireCommandAsync(std::move(submission), CommandKind::MaintenanceEnter, 0, "Library maintenance");

    if (!commandLeaseRes)
    {
      co_return std::unexpected{commandLeaseRes.error()};
    }

    auto commandLeasePtr = std::move(*commandLeaseRes);
    auto& owner = commandLeasePtr->owner();
    auto expected = LibraryAuthoringAvailability{};
    std::uint64_t generation = 0;
    bool closing = false;

    {
      auto const lock = std::scoped_lock{owner._stateMutex};
      closing = owner._lifecycle != Lifecycle::Open;

      if (!closing)
      {
        AO_INVARIANT(owner._activePhase == ActivePhase::PreTransaction);
        AO_INVARIANT(owner._state == LibraryAuthoringState::Available);
        owner._state = LibraryAuthoringState::Maintenance;
        generation = ++owner._maintenanceGeneration;
        expected = owner.availabilityLocked();
      }
    }

    if (closing)
    {
      commandLeasePtr.reset();
      async::throwOperationCancelled();
    }

    auto lifetimeStatePtr = std::weak_ptr<detail::LibraryMutationLifetimeState>{owner._lifetimeStatePtr};
    auto const delivered = co_await owner.deliverControlAvailabilityAsync(expected);
    commandLeasePtr.reset();

    if (!delivered)
    {
      async::throwOperationCancelled();
    }

    co_return MaintenanceGuard{std::move(lifetimeStatePtr), generation};
  }

  async::Task<void> LibraryMutationService::finishMaintenanceAsync(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation)
  {
    auto submission = Submission{lifetimeStatePtr, false};
    auto commandLeaseRes = co_await acquireCommandAsync(
      std::move(submission), CommandKind::MaintenanceExit, generation, "Library maintenance completion");

    if (!commandLeaseRes)
    {
      if (commandLeaseRes.error().code == Error::Code::InvalidState)
      {
        async::throwOperationCancelled();
      }

      abortLibraryInfrastructure("Library maintenance completion violated sequencer ordering");
    }

    auto commandLeasePtr = std::move(*commandLeaseRes);
    auto& owner = commandLeasePtr->owner();
    auto expected = LibraryAuthoringAvailability{};
    bool closing = false;

    {
      auto const lock = std::scoped_lock{owner._stateMutex};
      closing = owner._lifecycle != Lifecycle::Open;

      if (!closing)
      {
        AO_INVARIANT(owner._activePhase == ActivePhase::PreTransaction);
        AO_INVARIANT(owner._state == LibraryAuthoringState::Maintenance && generation == owner._maintenanceGeneration);
        owner._state = LibraryAuthoringState::Available;
        expected = owner.availabilityLocked();
      }
    }

    if (closing)
    {
      commandLeasePtr.reset();
      async::throwOperationCancelled();
    }

    auto const delivered = co_await owner.deliverControlAvailabilityAsync(expected);
    commandLeasePtr.reset();

    if (!delivered)
    {
      async::throwOperationCancelled();
    }
  }

  async::Task<bool> LibraryMutationService::deliverControlAvailabilityAsync(LibraryAuthoringAvailability expected)
  {
    auto deliveryPtr = std::make_shared<detail::LibraryMutationControlDelivery>(expected);
    auto deferredException = std::exception_ptr{};

    {
      auto const lock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase == ActivePhase::PreTransaction);
      AO_INVARIANT(_activeControlDeliveryPtr == nullptr);
      _activeControlDeliveryPtr = deliveryPtr;
      _activePhase = ActivePhase::AwaitingControlDelivery;
    }

    try
    {
      _callbackExecutor.dispatch(
        [weakLifetimeStatePtr = std::weak_ptr<detail::LibraryMutationLifetimeState>{_lifetimeStatePtr},
         deliveryPtr] noexcept
        {
          if (auto const lifetimeStatePtr = weakLifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
          {
            lifetimeStatePtr->invokeIfAlive([deliveryPtr](LibraryMutationService& owner) noexcept
                                            { owner.deliverControlAvailability(deliveryPtr); });
          }
        });
    }
    catch (...)
    {
      deferredException = std::current_exception();
    }

    if (deferredException)
    {
      bool alreadyRetired = false;

      {
        auto const lock = std::scoped_lock{_stateMutex};

        if (_lifecycle != Lifecycle::Open)
        {
          alreadyRetired = std::exchange(deliveryPtr->retired, true);
        }
        else
        {
          abortLibraryInfrastructure(
            "Live library maintenance could not reach the callback executor", deferredException);
        }
      }

      if (!alreadyRetired)
      {
        deliveryPtr->retire();
      }
    }

    auto const delivered = co_await deliveryPtr->wait();

    {
      auto const lock = std::scoped_lock{_stateMutex};

      if (_activeControlDeliveryPtr == deliveryPtr)
      {
        // Dispatch rejection or Closing may retire the delivery without
        // running its callback. Return the active control command to its
        // worker-side terminal phase before releasing its lane lease.
        AO_INVARIANT(_activePhase == ActivePhase::AwaitingControlDelivery);
        _activeControlDeliveryPtr.reset();
        _activePhase = ActivePhase::PreTransaction;
      }
      else
      {
        // Normal callback delivery makes this transition before signalling
        // the waiter, so Closing never observes an AwaitingControlDelivery
        // phase whose delivery pointer has already been cleared.
        AO_INVARIANT(_activeControlDeliveryPtr == nullptr);
        AO_INVARIANT(_activePhase == ActivePhase::PreTransaction);
      }
    }

    _stateChanged.notify_all();
    co_return delivered;
  }

  void LibraryMutationService::deliverControlAvailability(
    std::shared_ptr<detail::LibraryMutationControlDelivery> const& deliveryPtr) noexcept
  {
    {
      auto const lock = std::scoped_lock{_stateMutex};

      if (_activeControlDeliveryPtr != deliveryPtr || deliveryPtr->retired)
      {
        return;
      }

      deliveryPtr->claimed = true;
    }

    emitAvailability(deliveryPtr->expected);

    bool retired = false;

    {
      auto const lock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_activePhase == ActivePhase::AwaitingControlDelivery);
      AO_INVARIANT(_activeControlDeliveryPtr == deliveryPtr);
      retired = _lifecycle != Lifecycle::Open;
      deliveryPtr->retired = retired;
      _activeControlDeliveryPtr.reset();
      _activePhase = ActivePhase::PreTransaction;
    }

    if (retired)
    {
      deliveryPtr->retire();
    }
    else
    {
      deliveryPtr->complete();
    }

    _stateChanged.notify_all();
  }

  async::Task<Result<LibraryMutationService::Mutation>> LibraryMutationService::beginMaintenanceMutationAsync(
    Submission submission,
    MaintenanceGuard const& guard)
  {
    auto const lockedGuardStatePtr = guard._lifetimeStatePtr.lock();
    auto const submissionStatePtr = submission._lifetimeStatePtr.lock();

    if (lockedGuardStatePtr == nullptr || lockedGuardStatePtr != submissionStatePtr)
    {
      return async::makeReadyTask(
        Result<Mutation>{makeError(Error::Code::InvalidState, "Library maintenance session is no longer active")});
    }

    return beginMutationAsync(
      std::move(submission), CommandKind::MaintenanceMutation, guard._generation, {}, "Library maintenance mutation");
  }

  void LibraryMutationService::handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};

      if (_lifecycle != Lifecycle::Open)
      {
        return;
      }
    }

    abortLibraryInfrastructure(
      "Live library task finalization was rejected by the callback executor", std::move(exceptionPtr));
  }

  std::optional<LibraryMutationService::PublicationDiagnosticContext>
  LibraryMutationService::activePublicationDiagnosticContext() const noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};
    return _optActivePublicationDiagnosticContext;
  }

  bool LibraryMutationService::beginAvailabilityNotification(LibraryAuthoringAvailability const& expected) noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};

    if (_lifecycle != Lifecycle::Open || availabilityLocked() != expected)
    {
      return false;
    }

    AO_INVARIANT(!_availabilityNotificationInProgress);
    _availabilityNotificationInProgress = true;
    return true;
  }

  void LibraryMutationService::completeAvailabilityNotification() noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_availabilityNotificationInProgress);
      _availabilityNotificationInProgress = false;
    }

    _stateChanged.notify_all();
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

  bool LibraryMutationService::submissionIsReentrantLocked() const noexcept
  {
    return _availabilityNotificationInProgress || _changes.publicationDeliveryInProgressFromCoordinator();
  }

  bool LibraryMutationService::hasOutstandingCommandLocked(CommandKind const kind) const noexcept
  {
    auto const matchesKind = [kind](auto const& requestPtr) { return requestPtr->kind == kind; };
    return (_activeCommandRequestPtr != nullptr && matchesKind(_activeCommandRequestPtr)) ||
           std::ranges::any_of(_commandQueue, matchesKind);
  }

  bool LibraryMutationService::hasMaintenanceTransitionLocked() const noexcept
  {
    return hasOutstandingCommandLocked(CommandKind::MaintenanceEnter) ||
           hasOutstandingCommandLocked(CommandKind::MaintenanceExit);
  }

  LibraryAuthoringAvailability LibraryMutationService::availabilityLocked() const noexcept
  {
    return LibraryAuthoringAvailability{
      .state = _state, .runtimeInstanceId = _runtimeInstanceId, .libraryRevision = _availableRevision};
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity): Closing mirrors the explicit command-phase teardown.
  void LibraryMutationService::beginClosing() noexcept
  {
    auto retiredRequests = std::vector<std::shared_ptr<detail::LibraryMutationCommandRequest>>{};
    auto activeRequestPtr = std::shared_ptr<detail::LibraryMutationCommandRequest>{};

    {
      auto const lock = std::scoped_lock{_stateMutex};

      if (_lifecycle == Lifecycle::Closed)
      {
        return;
      }

      if (_lifecycle == Lifecycle::Open)
      {
        _lifecycle = Lifecycle::Closing;
        retiredRequests.assign(
          std::make_move_iterator(_commandQueue.begin()), std::make_move_iterator(_commandQueue.end()));
        _commandQueue.clear();

        if (_activePhase == ActivePhase::PreTransaction || _activePhase == ActivePhase::InTransaction)
        {
          AO_INVARIANT(_activeCommandRequestPtr != nullptr);
          activeRequestPtr = _activeCommandRequestPtr;
        }
      }
    }

    for (auto const& requestPtr : retiredRequests)
    {
      requestPtr->retire();
    }

    if (activeRequestPtr != nullptr)
    {
      activeRequestPtr->requestClosingStop();
    }

    while (true)
    {
      auto controlDeliveryPtr = std::shared_ptr<detail::LibraryMutationControlDelivery>{};
      bool sealChanges = false;
      bool closed = false;

      {
        auto stateLock = std::unique_lock{_stateMutex};

        if (_activePhase == ActivePhase::AwaitingPublication && !_changesSealed)
        {
          _changesSealed = true;
          sealChanges = true;
        }
        else if (_activePhase == ActivePhase::AwaitingControlDelivery)
        {
          AO_INVARIANT(_activeControlDeliveryPtr != nullptr);

          if (!_activeControlDeliveryPtr->claimed && !_activeControlDeliveryPtr->retired)
          {
            _activeControlDeliveryPtr->retired = true;
            controlDeliveryPtr = _activeControlDeliveryPtr;
          }
          else if (_activeControlDeliveryPtr->claimed)
          {
            AO_EXPECTS(
              !_callbackExecutor.isCurrent(), "Library closing cannot run inside an active availability observer");
          }
        }
        else if (_activePhase == ActivePhase::Idle)
        {
          if (!_changesSealed)
          {
            _changesSealed = true;
            sealChanges = true;
          }
          else
          {
            AO_INVARIANT(_commandQueue.empty());
            AO_INVARIANT(_activeCommandRequestPtr == nullptr);
            AO_INVARIANT(_activePublicationEventPtr == nullptr);
            AO_INVARIANT(_activeControlDeliveryPtr == nullptr);
            _state = LibraryAuthoringState::Available;
            _optBackgroundTaskKind.reset();
            _lifecycle = Lifecycle::Closed;
            closed = true;
          }
        }

        if (!sealChanges && controlDeliveryPtr == nullptr && !closed)
        {
          _stateChanged.wait(stateLock);
          continue;
        }
      }

      if (sealChanges)
      {
        _changes.sealAndRetireFromCoordinator();
        _stateChanged.notify_all();
      }

      if (controlDeliveryPtr != nullptr)
      {
        controlDeliveryPtr->retire();
        _stateChanged.notify_all();
      }

      if (closed)
      {
        break;
      }
    }

    _lifetimeStatePtr->retire();
  }
} // namespace ao::rt
