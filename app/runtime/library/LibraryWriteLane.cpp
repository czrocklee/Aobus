// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryWriteLane.h"

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
    LibraryMutationOwnerLease(LibraryMutationOwnerLease&& other) noexcept;
    LibraryMutationOwnerLease& operator=(LibraryMutationOwnerLease&&) = delete;

    LibraryWriteLane& owner() const noexcept { return *_owner; }
    bool ownerIsOpen() const noexcept;
    void releaseCommand(std::shared_ptr<LibraryMutationCommandRequest> const& requestPtr) noexcept;
    LibraryWriteLane* releaseOwner() noexcept { return std::exchange(_owner, nullptr); }
    std::shared_ptr<LibraryMutationLifetimeState> releaseState() noexcept { return std::move(_statePtr); }

  private:
    LibraryMutationOwnerLease(std::shared_ptr<LibraryMutationLifetimeState> statePtr, LibraryWriteLane& owner) noexcept
      : _statePtr{std::move(statePtr)}, _owner{&owner}
    {
    }

    std::shared_ptr<LibraryMutationLifetimeState> _statePtr;
    LibraryWriteLane* _owner;

    friend class LibraryMutationLifetimeState;
  };

  class detail::LibraryMutationLifetimeState final : public std::enable_shared_from_this<LibraryMutationLifetimeState>
  {
  public:
    explicit LibraryMutationLifetimeState(LibraryWriteLane* ownerValue) noexcept
      : _owner{ownerValue}
    {
    }

    std::optional<LibraryMutationOwnerLease> acquire()
    {
      auto statePtr = shared_from_this();
      auto const lock = std::scoped_lock{_mutex};

      if (_owner == nullptr)
      {
        return std::nullopt;
      }

      ++_activeCalls;
      return LibraryMutationOwnerLease{std::move(statePtr), *_owner};
    }

    template<typename Operation>
    void invokeIfAlive(Operation&& operation)
    {
      if (auto optOwnerLease = acquire(); optOwnerLease)
      {
        std::invoke(std::forward<Operation>(operation), optOwnerLease->owner());
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
    LibraryWriteLane* _owner;
    std::size_t _activeCalls = 0;

    friend class LibraryMutationOwnerLease;
    friend class LibraryWriteLane::Mutation;
  };

  namespace detail
  {
    LibraryMutationOwnerLease::~LibraryMutationOwnerLease()
    {
      if (_owner != nullptr)
      {
        _statePtr->releaseCall();
      }
    }

    LibraryMutationOwnerLease::LibraryMutationOwnerLease(LibraryMutationOwnerLease&& other) noexcept
      : _statePtr{std::move(other._statePtr)}, _owner{std::exchange(other._owner, nullptr)}
    {
    }

    bool LibraryMutationOwnerLease::ownerIsOpen() const noexcept
    {
      auto const lock = std::scoped_lock{_owner->_stateMutex};
      return _owner->_lifecycle == LibraryWriteLane::Lifecycle::Open;
    }

    void LibraryMutationOwnerLease::releaseCommand(
      std::shared_ptr<LibraryMutationCommandRequest> const& requestPtr) noexcept
    {
      _owner->releaseCommand(requestPtr);
    }
  } // namespace detail

  struct detail::LibraryMutationCommandRequest final
  {
    explicit LibraryMutationCommandRequest(LibraryWriteLane::CommandKind const commandKind)
      : kind{commandKind}
    {
    }

    async::Task<bool> wait() const { return event.wait(); }
    void grant() noexcept { event.signal(true); }
    void retire() noexcept { event.signal(false); }
    std::stop_token closingStopToken() const noexcept { return closingStopSource.get_token(); }
    void requestClosingStop() const noexcept { std::ignore = closingStopSource.request_stop(); }

    LibraryWriteLane::CommandKind kind;
    OneShotEvent<bool> event;
    mutable std::stop_source closingStopSource;
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

  namespace
  {
    struct CommandAdmission final
    {
      LibraryWriteLane* owner;
      std::shared_ptr<detail::LibraryMutationCommandRequest> requestPtr;
      std::shared_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr;
    };

    class [[nodiscard]] CommandAdmissionGuard final
    {
    public:
      CommandAdmissionGuard(detail::LibraryMutationOwnerLease ownerPermit,
                            std::shared_ptr<detail::LibraryMutationCommandRequest> requestPtr) noexcept
        : _ownerPermit{std::move(ownerPermit)}, _requestPtr{std::move(requestPtr)}
      {
      }

      ~CommandAdmissionGuard()
      {
        if (_granted)
        {
          _ownerPermit.releaseCommand(_requestPtr);
        }
      }

      CommandAdmissionGuard(CommandAdmissionGuard const&) = delete;
      CommandAdmissionGuard& operator=(CommandAdmissionGuard const&) = delete;
      CommandAdmissionGuard(CommandAdmissionGuard&& other) noexcept
        : _ownerPermit{std::move(other._ownerPermit)}
        , _requestPtr{std::move(other._requestPtr)}
        , _granted{std::exchange(other._granted, false)}
      {
      }
      CommandAdmissionGuard& operator=(CommandAdmissionGuard&&) = delete;

      LibraryWriteLane& owner() const noexcept { return _ownerPermit.owner(); }
      bool ownerIsOpen() const noexcept { return _ownerPermit.ownerIsOpen(); }
      std::stop_token closingStopToken() const noexcept { return _requestPtr->closingStopToken(); }
      async::Task<bool> wait() const { return _requestPtr->wait(); }
      void markGranted() noexcept { _granted = true; }

      CommandAdmission release() noexcept
      {
        AO_INVARIANT(_granted);
        _granted = false;
        auto admission = CommandAdmission{
          .owner = _ownerPermit.releaseOwner(),
          .requestPtr = std::move(_requestPtr),
          .lifetimeStatePtr = _ownerPermit.releaseState(),
        };
        return admission;
      }

    private:
      detail::LibraryMutationOwnerLease _ownerPermit;
      std::shared_ptr<detail::LibraryMutationCommandRequest> _requestPtr;
      bool _granted = false;
    };

    using EnqueueCommand =
      compat::MoveOnlyFunction<Result<std::shared_ptr<detail::LibraryMutationCommandRequest>>(LibraryWriteLane&)>;

    async::Task<Result<CommandAdmissionGuard>> acquireCommandAsync(
      std::weak_ptr<detail::LibraryMutationLifetimeState> weakLifetimeStatePtr,
      EnqueueCommand enqueueCommand,
      std::string operation)
    {
      auto lifetimeStatePtr = weakLifetimeStatePtr.lock();

      if (lifetimeStatePtr == nullptr)
      {
        co_return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
      }

      auto optOwnerPermit = lifetimeStatePtr->acquire();

      if (!optOwnerPermit)
      {
        co_return makeError(Error::Code::InvalidState, std::format("{} is unavailable", operation));
      }

      auto requestRes = enqueueCommand(optOwnerPermit->owner());

      if (!requestRes)
      {
        co_return std::unexpected{requestRes.error()};
      }

      auto guard = CommandAdmissionGuard{std::move(*optOwnerPermit), std::move(*requestRes)};

      if (!co_await guard.wait())
      {
        async::throwOperationCancelled();
      }

      guard.markGranted();

      if (!guard.ownerIsOpen())
      {
        async::throwOperationCancelled();
      }

      co_return std::move(guard);
    }
  } // namespace

  LibraryWriteLane::Submission::Submission(std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
                                           bool const reentrant) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _reentrant{reentrant}
  {
  }

  LibraryWriteLane::Mutation::Mutation(LibraryWriteLane& owner,
                                       std::shared_ptr<detail::LibraryMutationCommandRequest> requestPtr,
                                       std::shared_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
                                       library::WriteTransaction transaction) noexcept
    : _owner{&owner}
    , _requestPtr{std::move(requestPtr)}
    , _lifetimeStatePtr{std::move(lifetimeStatePtr)}
    , _transaction{std::move(transaction)}
  {
  }

  LibraryWriteLane::Mutation::~Mutation()
  {
    abort();
  }

  LibraryWriteLane::Mutation::Mutation(Mutation&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}
    , _requestPtr{std::move(other._requestPtr)}
    , _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}
    , _transaction{std::move(other._transaction)}
    , _terminal{std::exchange(other._terminal, true)}
    , _executeEligible{std::exchange(other._executeEligible, false)}
  {
  }

  void LibraryWriteLane::Mutation::finish() noexcept
  {
    _terminal = true;
    releaseAdmission();
  }

  void LibraryWriteLane::Mutation::releaseAdmission() noexcept
  {
    if (_owner == nullptr)
    {
      return;
    }

    auto* const owner = std::exchange(_owner, nullptr);
    owner->releaseCommand(_requestPtr);
    _requestPtr.reset();
    _lifetimeStatePtr->releaseCall();
    _lifetimeStatePtr.reset();
  }

  std::stop_token LibraryWriteLane::Mutation::closingStopToken() const noexcept
  {
    AO_INVARIANT(_owner != nullptr, "Terminal library mutation has no Closing stop token");
    return _requestPtr->closingStopToken();
  }

  void LibraryWriteLane::Mutation::abort() noexcept
  {
    if (!_terminal)
    {
      _terminal = true;
      _transaction.abort();
    }

    releaseAdmission();
  }

  LibraryWriteLane::MaintenanceGuard::MaintenanceGuard(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _generation{generation}
  {
  }

  LibraryWriteLane::MaintenanceGuard::~MaintenanceGuard()
  {
    auto const generation = std::exchange(_generation, 0);

    if (generation == 0)
    {
      return;
    }

    if (auto const lifetimeStatePtr = _lifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
    {
      lifetimeStatePtr->invokeIfAlive(
        [generation](LibraryWriteLane& owner)
        {
          auto const lock = std::scoped_lock{owner._stateMutex};
          AO_INVARIANT(owner._lifecycle != Lifecycle::Open || generation != owner._maintenanceGeneration,
                       "Live library maintenance guard was destroyed without awaiting finishAsync");
        });
    }
  }

  LibraryWriteLane::MaintenanceGuard::MaintenanceGuard(MaintenanceGuard&& other) noexcept
    : _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  async::Task<void> LibraryWriteLane::MaintenanceGuard::finishAsync()
  {
    auto const generation = std::exchange(_generation, 0);
    AO_EXPECTS(generation != 0, "Library maintenance guard has already finished");
    return LibraryWriteLane::finishMaintenanceAsync(_lifetimeStatePtr, generation);
  }

  LibraryWriteLane::BackgroundTaskLease::BackgroundTaskLease(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation) noexcept
    : _lifetimeStatePtr{std::move(lifetimeStatePtr)}, _generation{generation}
  {
  }

  LibraryWriteLane::BackgroundTaskLease::~BackgroundTaskLease()
  {
    finish();
  }

  LibraryWriteLane::BackgroundTaskLease::BackgroundTaskLease(BackgroundTaskLease&& other) noexcept
    : _lifetimeStatePtr{std::move(other._lifetimeStatePtr)}, _generation{std::exchange(other._generation, 0)}
  {
  }

  void LibraryWriteLane::BackgroundTaskLease::finish() noexcept
  {
    auto const generation = std::exchange(_generation, 0);

    if (generation == 0)
    {
      return;
    }

    if (auto const lifetimeStatePtr = _lifetimeStatePtr.lock(); lifetimeStatePtr != nullptr)
    {
      lifetimeStatePtr->invokeIfAlive([generation](LibraryWriteLane& owner) noexcept
                                      { owner.finishBackgroundTask(generation); });
    }
  }

  LibraryWriteLane::LibraryWriteLane(async::Executor& callbackExecutor,
                                     library::WritableMusicLibrary writableLibrary,
                                     LibraryChanges& changes,
                                     WriteTransactionFactory writeTransactionFactory)
    : _callbackExecutor{callbackExecutor}
    , _writableLibrary{std::move(writableLibrary)}
    , _library{_writableLibrary.library()}
    , _changes{changes}
    , _writeTransactionFactory{std::move(writeTransactionFactory)}
    , _runtimeInstanceId{nextRuntimeInstanceId()}
    , _lifetimeStatePtr{std::make_shared<detail::LibraryMutationLifetimeState>(this)}
    , _lastCommittedRevision{currentLibraryRevision(_library)}
    , _availableRevision{_lastCommittedRevision}
  {
  }

  LibraryWriteLane::~LibraryWriteLane()
  {
    beginClosing();
  }

  LibraryAuthoringAvailability LibraryWriteLane::availability() const
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return availabilityLocked();
  }

  async::Subscription LibraryWriteLane::onAvailabilityChanged(
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

  Result<BoundTrackTargets> LibraryWriteLane::bindTrackTargets(std::span<TrackId const> const trackIds) const
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

  Result<BoundListOrder> LibraryWriteLane::bindListOrder(ListId const listId,
                                                         std::span<TrackId const> const effectiveTrackIds) const
  {
    return bindListOrder(listId, std::vector<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()});
  }

  Result<BoundListOrder> LibraryWriteLane::bindListOrder(ListId const listId,
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

  BoundTrackTargets LibraryWriteLane::advanceBoundTargets(BoundTrackTargets const& targets,
                                                          std::uint64_t const revision)
  {
    return BoundTrackTargets{
      targets._stamp.runtimeId, revision, std::vector<TrackId>{targets._trackIds.begin(), targets._trackIds.end()}};
  }

  LibraryWriteLane::Submission LibraryWriteLane::captureSubmission() const noexcept
  {
    auto const lock = std::scoped_lock{_stateMutex};
    return Submission{_lifetimeStatePtr, _callbackExecutor.isCurrent() && submissionIsReentrantLocked()};
  }

  Result<std::shared_ptr<detail::LibraryMutationCommandRequest>> LibraryWriteLane::enqueueCommand(
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

  async::Task<Result<LibraryWriteLane::Mutation>> LibraryWriteLane::beginMutationAsync(
    Submission submission,
    CommandKind const kind,
    std::uint64_t const generation,
    library::WriteTransaction::Options options,
    std::string operation,
    compat::MoveOnlyFunction<Result<>(std::stop_token)> preTransaction)
  {
    auto commandGuardRes = co_await acquireCommandAsync(
      submission._lifetimeStatePtr,
      EnqueueCommand{[kind, generation, reentrant = submission._reentrant, operation](LibraryWriteLane& owner)
                     { return owner.enqueueCommand(kind, generation, reentrant, operation); }},
      operation);

    if (!commandGuardRes)
    {
      co_return std::unexpected{commandGuardRes.error()};
    }

    auto commandGuard = std::move(*commandGuardRes);
    auto& owner = commandGuard.owner();

    if (preTransaction)
    {
      auto preTransactionRes = preTransaction(commandGuard.closingStopToken());

      if (!preTransactionRes)
      {
        co_return std::unexpected{preTransactionRes.error()};
      }
    }

    if (!owner.beginTransaction())
    {
      async::throwOperationCancelled();
    }

    // The factory is immutable after construction and lane admission serializes
    // invocation, so worker access does not require _stateMutex.
    auto transaction = owner._writeTransactionFactory
                         ? owner._writeTransactionFactory(owner._writableLibrary, std::move(options))
                         : owner._writableLibrary.writeTransaction(std::move(options));
    auto admission = commandGuard.release();
    co_return Mutation{
      *admission.owner, std::move(admission.requestPtr), std::move(admission.lifetimeStatePtr), std::move(transaction)};
  }

  async::Task<Result<LibraryWriteLane::Mutation>> LibraryWriteLane::beginInteractiveMutationAsync(
    Submission submission,
    library::WriteTransaction::Options options)
  {
    return beginMutationAsync(
      std::move(submission), CommandKind::Interactive, 0, std::move(options), "Library mutation");
  }

  async::Task<LibraryWriteLane::AuthoringStart> LibraryWriteLane::beginAuthoringMutationAsync(Submission submission,
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

  async::Task<LibraryWriteLane::AuthoringStart> LibraryWriteLane::beginListOrderAuthoringMutationAsync(
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

  Result<LibraryWriteLane::BackgroundTaskLease> LibraryWriteLane::beginBackgroundTask(BackgroundTaskKind const kind)
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

  async::Task<Result<LibraryWriteLane::Mutation>> LibraryWriteLane::beginBackgroundMutationAsync(
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

  bool LibraryWriteLane::beginTransaction() noexcept
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

  Result<std::uint64_t> LibraryWriteLane::commitMutation(Mutation& mutation, LibraryChangeSet changeSet)
  {
    AO_EXPECTS(mutation._owner == this && mutation._requestPtr != nullptr && !mutation._terminal,
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
                LibraryWriteLane& owner) mutable
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

  async::Task<detail::LibraryPublicationTerminal> LibraryWriteLane::settleMutationAsync(std::uint64_t const revision)
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

  [[noreturn]] void LibraryWriteLane::abortPostCommitSettlement(std::exception_ptr const exceptionPtr) noexcept
  {
    abortLibraryInfrastructure("Committed library mutation could not reach publication settlement", exceptionPtr);
  }

  void LibraryWriteLane::completeCommittedCommand(std::uint64_t const revision) noexcept
  {
    auto const lock = std::scoped_lock{_stateMutex};
    AO_INVARIANT(_activePhase == ActivePhase::AwaitingPublication);
    AO_INVARIANT(_activePublicationEventPtr != nullptr && _activePublicationEventPtr->revision == revision);
    _activePublicationEventPtr.reset();
  }

  void LibraryWriteLane::finishPublication(detail::LibraryPublicationTerminal const terminal,
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

  void LibraryWriteLane::releaseCommand(
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

  void LibraryWriteLane::finishBackgroundTask(std::uint64_t const generation) noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};

    if (_lifecycle != Lifecycle::Open || generation != _backgroundTaskGeneration)
    {
      return;
    }

    _optBackgroundTaskKind.reset();
  }

  async::Task<Result<LibraryWriteLane::MaintenanceGuard>> LibraryWriteLane::beginMaintenanceAsync(Submission submission)
  {
    auto commandGuardRes = co_await acquireCommandAsync(
      submission._lifetimeStatePtr,
      EnqueueCommand{
        [reentrant = submission._reentrant](LibraryWriteLane& owner)
        { return owner.enqueueCommand(CommandKind::MaintenanceEnter, 0, reentrant, "Library maintenance"); }},
      "Library maintenance");

    if (!commandGuardRes)
    {
      co_return std::unexpected{commandGuardRes.error()};
    }

    auto commandGuard = std::move(*commandGuardRes);
    auto& owner = commandGuard.owner();
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
      async::throwOperationCancelled();
    }

    auto lifetimeStatePtr = std::weak_ptr<detail::LibraryMutationLifetimeState>{owner._lifetimeStatePtr};
    auto const delivered = co_await owner.deliverControlAvailabilityAsync(expected);

    if (!delivered)
    {
      async::throwOperationCancelled();
    }

    co_return MaintenanceGuard{std::move(lifetimeStatePtr), generation};
  }

  async::Task<void> LibraryWriteLane::finishMaintenanceAsync(
    std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
    std::uint64_t const generation)
  {
    auto commandGuardRes = co_await acquireCommandAsync(
      lifetimeStatePtr,
      EnqueueCommand{[generation](LibraryWriteLane& owner)
                     {
                       return owner.enqueueCommand(
                         CommandKind::MaintenanceExit, generation, false, "Library maintenance completion");
                     }},
      "Library maintenance completion");

    if (!commandGuardRes)
    {
      if (commandGuardRes.error().code == Error::Code::InvalidState)
      {
        async::throwOperationCancelled();
      }

      abortLibraryInfrastructure("Library maintenance completion violated sequencer ordering");
    }

    auto commandGuard = std::move(*commandGuardRes);
    auto& owner = commandGuard.owner();
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
      async::throwOperationCancelled();
    }

    auto const delivered = co_await owner.deliverControlAvailabilityAsync(expected);

    if (!delivered)
    {
      async::throwOperationCancelled();
    }
  }

  async::Task<bool> LibraryWriteLane::deliverControlAvailabilityAsync(LibraryAuthoringAvailability expected)
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
            lifetimeStatePtr->invokeIfAlive([deliveryPtr](LibraryWriteLane& owner) noexcept
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

  void LibraryWriteLane::deliverControlAvailability(
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

  async::Task<Result<LibraryWriteLane::Mutation>> LibraryWriteLane::beginMaintenanceMutationAsync(
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

  void LibraryWriteLane::handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept
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

  std::optional<LibraryWriteLane::PublicationDiagnosticContext> LibraryWriteLane::activePublicationDiagnosticContext()
    const noexcept
  {
    auto const stateLock = std::scoped_lock{_stateMutex};
    return _optActivePublicationDiagnosticContext;
  }

  bool LibraryWriteLane::beginAvailabilityNotification(LibraryAuthoringAvailability const& expected) noexcept
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

  void LibraryWriteLane::completeAvailabilityNotification() noexcept
  {
    {
      auto const stateLock = std::scoped_lock{_stateMutex};
      AO_INVARIANT(_availabilityNotificationInProgress);
      _availabilityNotificationInProgress = false;
    }

    _stateChanged.notify_all();
  }

  void LibraryWriteLane::emitAvailability(LibraryAuthoringAvailability const& expected) noexcept
  {
    if (!beginAvailabilityNotification(expected))
    {
      return;
    }

    _availabilityChanged.emit(expected);
    completeAvailabilityNotification();
  }

  bool LibraryWriteLane::submissionIsReentrantLocked() const noexcept
  {
    return _availabilityNotificationInProgress || _changes.publicationDeliveryInProgressFromCoordinator();
  }

  bool LibraryWriteLane::hasOutstandingCommandLocked(CommandKind const kind) const noexcept
  {
    auto const matchesKind = [kind](auto const& requestPtr) { return requestPtr->kind == kind; };
    return (_activeCommandRequestPtr != nullptr && matchesKind(_activeCommandRequestPtr)) ||
           std::ranges::any_of(_commandQueue, matchesKind);
  }

  bool LibraryWriteLane::hasMaintenanceTransitionLocked() const noexcept
  {
    return hasOutstandingCommandLocked(CommandKind::MaintenanceEnter) ||
           hasOutstandingCommandLocked(CommandKind::MaintenanceExit);
  }

  LibraryAuthoringAvailability LibraryWriteLane::availabilityLocked() const noexcept
  {
    return LibraryAuthoringAvailability{
      .state = _state, .runtimeInstanceId = _runtimeInstanceId, .libraryRevision = _availableRevision};
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity): Closing mirrors the explicit command-phase teardown.
  void LibraryWriteLane::beginClosing() noexcept
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
