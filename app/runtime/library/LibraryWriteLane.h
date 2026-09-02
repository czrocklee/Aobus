// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::async
{
  class Executor;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  template<typename Value>
  struct Unchanged final
  {
    Value value;
  };

  template<typename Value>
  struct Changed final
  {
    Value value;
    LibraryChangeSet changeSet;
  };

  template<typename Value>
  using OperationOutcome = std::variant<Unchanged<Value>, Changed<Value>>;

  template<typename Value>
  struct MutationExecution final
  {
    Value value;
    std::optional<std::uint64_t> optCommittedRevision;
  };

  namespace detail
  {
    class LibraryMutationLifetimeState;
    class LibraryMutationOwnerLease;
    struct LibraryMutationCommandRequest;
    struct LibraryMutationControlDelivery;
    struct LibraryMutationPublicationEvent;

    template<typename Type>
    struct OperationResultTraits final
    {
      static constexpr bool kValid = false;
    };

    template<typename Value>
    struct OperationResultTraits<Result<std::variant<Unchanged<Value>, Changed<Value>>>> final
    {
      static constexpr bool kValid = true;
      using ValueType = Value;
    };

    void requireMatchingPublicationCompletion(bool publicationInProgress,
                                              std::uint64_t revision,
                                              std::uint64_t committedRevision,
                                              std::string_view libraryIdentity,
                                              std::string_view replicaName);
  } // namespace detail

  class LibraryWriteLane final
  {
  public:
    enum class BackgroundTaskKind : std::uint8_t
    {
      Import,
      ScanApply,
      AudioIdentityBackfill,
    };

    class Submission final
    {
    private:
      Submission(std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr, bool reentrant) noexcept;

      std::weak_ptr<detail::LibraryMutationLifetimeState> _lifetimeStatePtr;
      bool _reentrant = false;

      friend class LibraryWriteLane;
    };

    class [[nodiscard]] Mutation final
    {
    public:
      ~Mutation();

      Mutation(Mutation const&) = delete;
      Mutation& operator=(Mutation const&) = delete;
      Mutation(Mutation&& other) noexcept;
      Mutation& operator=(Mutation&& other) = delete;

      template<typename Function,
               typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Function, library::LibraryWrite&>>>
        requires library::detail::IsResult<OperationResult>::value
      OperationResult apply(Function&& function)
      {
        AO_INVARIANT(_owner != nullptr && !_terminal, "Library mutation is already terminal");
        _executeEligible = false;

        try
        {
          auto result = _transaction.apply(std::forward<Function>(function));

          if (!result)
          {
            abort();
          }

          return result;
        }
        catch (...)
        {
          abort();
          throw;
        }
      }

      template<typename Operation,
               typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>>
        requires detail::OperationResultTraits<OperationResult>::kValid
      async::Task<Result<MutationExecution<typename detail::OperationResultTraits<OperationResult>::ValueType>>>
      executeAsync(Operation operation, std::string operationName = "Library mutation")
      {
        using Value = detail::OperationResultTraits<OperationResult>::ValueType;
        using Execution = MutationExecution<Value>;

        static_assert(std::is_nothrow_move_constructible_v<Value>);
        static_assert(std::is_nothrow_move_constructible_v<LibraryChangeSet>);
        AO_INVARIANT(_owner != nullptr && !_terminal, "Library mutation is already terminal");
        AO_INVARIANT(_executeEligible, "Library mutation execute must own the first write operation");
        _executeEligible = false;

        auto execution = Execution{};
        auto deferredException = std::exception_ptr{};

        try
        {
          auto outcomeRes = _transaction.apply(std::move(operation));

          if (!outcomeRes)
          {
            auto error = std::move(outcomeRes.error());
            abort();
            co_return std::unexpected{std::move(error)};
          }

          if (auto* unchanged = std::get_if<Unchanged<Value>>(&*outcomeRes); unchanged != nullptr)
          {
            execution.value = std::move(unchanged->value);
            abort();
            co_return Result<Execution>{std::move(execution)};
          }

          auto changed = std::get<Changed<Value>>(std::move(*outcomeRes));
          execution.value = std::move(changed.value);
          auto commitRes = _owner->commitMutation(*this, std::move(changed.changeSet));

          if (!commitRes)
          {
            auto error = std::move(commitRes.error());
            error.message = std::format("{} commit failed: {}", operationName, error.message);
            co_return std::unexpected{std::move(error)};
          }

          execution.optCommittedRevision = *commitRes;
        }
        catch (...)
        {
          deferredException = std::current_exception();
          abort();
          async::rethrowException(deferredException);
        }

        auto terminal = detail::LibraryPublicationTerminal{};

        try
        {
          terminal = co_await _owner->settleMutationAsync(*execution.optCommittedRevision);
        }
        catch (...)
        {
          deferredException = std::current_exception();
          // A durable commit cannot abandon publication settlement, including on cancellation.
          _owner->abortPostCommitSettlement(deferredException);
        }

        finish();

        if (terminal == detail::LibraryPublicationTerminal::RetiredByClosing)
        {
          async::throwOperationCancelled();
        }

        co_return Result<Execution>{std::move(execution)};
      }

      std::stop_token closingStopToken() const noexcept;
      void abort() noexcept;

    private:
      Mutation(LibraryWriteLane& owner,
               std::shared_ptr<detail::LibraryMutationCommandRequest> requestPtr,
               std::shared_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
               library::WriteTransaction transaction) noexcept;
      void finish() noexcept;
      void releaseAdmission() noexcept;

      LibraryWriteLane* _owner = nullptr;
      std::shared_ptr<detail::LibraryMutationCommandRequest> _requestPtr;
      std::shared_ptr<detail::LibraryMutationLifetimeState> _lifetimeStatePtr;
      library::WriteTransaction _transaction;
      bool _terminal = false;
      bool _executeEligible = true;

      friend class LibraryWriteLane;
    };

    class [[nodiscard]] MaintenanceGuard final
    {
    public:
      ~MaintenanceGuard();

      MaintenanceGuard(MaintenanceGuard const&) = delete;
      MaintenanceGuard& operator=(MaintenanceGuard const&) = delete;
      MaintenanceGuard(MaintenanceGuard&& other) noexcept;
      MaintenanceGuard& operator=(MaintenanceGuard&&) = delete;

      async::Task<void> finishAsync();

    private:
      MaintenanceGuard(std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
                       std::uint64_t generation) noexcept;

      std::weak_ptr<detail::LibraryMutationLifetimeState> _lifetimeStatePtr;
      std::uint64_t _generation = 0;

      friend class LibraryWriteLane;
    };

    class [[nodiscard]] BackgroundTaskLease final
    {
    public:
      ~BackgroundTaskLease();

      BackgroundTaskLease(BackgroundTaskLease const&) = delete;
      BackgroundTaskLease& operator=(BackgroundTaskLease const&) = delete;
      BackgroundTaskLease(BackgroundTaskLease&& other) noexcept;
      BackgroundTaskLease& operator=(BackgroundTaskLease&&) = delete;

      void finish() noexcept;

    private:
      BackgroundTaskLease(std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
                          std::uint64_t generation) noexcept;

      std::weak_ptr<detail::LibraryMutationLifetimeState> _lifetimeStatePtr;
      std::uint64_t _generation = 0;

      friend class LibraryWriteLane;
    };

    struct AuthoringStart final
    {
      AuthoringStatus status = AuthoringStatus::Unavailable;
      std::optional<Mutation> optMutation{};
    };

    // Test seam for forcing write-transaction construction failure before
    // command admission transfers to Mutation; production leaves it empty.
    using WriteTransactionFactory =
      compat::MoveOnlyFunction<library::WriteTransaction(library::WritableMusicLibrary&,
                                                         library::WriteTransaction::Options)>;

    LibraryWriteLane(async::Executor& callbackExecutor,
                     library::WritableMusicLibrary writableLibrary,
                     LibraryChanges& changes,
                     WriteTransactionFactory writeTransactionFactory = {});
    ~LibraryWriteLane();

    LibraryWriteLane(LibraryWriteLane const&) = delete;
    LibraryWriteLane& operator=(LibraryWriteLane const&) = delete;
    LibraryWriteLane(LibraryWriteLane&&) = delete;
    LibraryWriteLane& operator=(LibraryWriteLane&&) = delete;

    LibraryAuthoringAvailability availability() const;
    async::Subscription onAvailabilityChanged(
      compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::span<TrackId const> effectiveTrackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::vector<TrackId>&& effectiveTrackIds) const;
    static BoundTrackTargets advanceBoundTargets(BoundTrackTargets const& targets, std::uint64_t revision);

    Submission captureSubmission() const noexcept;
    static async::Task<Result<Mutation>> beginInteractiveMutationAsync(Submission submission,
                                                                       library::WriteTransaction::Options options = {});
    static async::Task<AuthoringStart> beginAuthoringMutationAsync(Submission submission, BoundTrackTargets targets);
    static async::Task<AuthoringStart> beginListOrderAuthoringMutationAsync(Submission submission,
                                                                            BoundListOrder order);
    Result<BackgroundTaskLease> beginBackgroundTask(BackgroundTaskKind kind);
    static async::Task<Result<Mutation>> beginBackgroundMutationAsync(
      Submission submission,
      BackgroundTaskLease const& lease,
      compat::MoveOnlyFunction<Result<>(std::stop_token)> preTransaction = {});
    static async::Task<Result<MaintenanceGuard>> beginMaintenanceAsync(Submission submission);
    static async::Task<Result<Mutation>> beginMaintenanceMutationAsync(Submission submission,
                                                                       MaintenanceGuard const& guard);

  private:
    enum class Lifecycle : std::uint8_t
    {
      Open,
      Closing,
      Closed,
    };

    enum class ActivePhase : std::uint8_t
    {
      Idle,
      PreTransaction,
      InTransaction,
      SubmittingPublication,
      AwaitingPublication,
      AwaitingControlDelivery,
    };

    enum class CommandKind : std::uint8_t
    {
      Interactive,
      Background,
      MaintenanceMutation,
      MaintenanceEnter,
      MaintenanceExit,
    };

    struct PublicationDiagnosticContext final
    {
      std::string libraryIdentity;
      std::string replicaName;
      std::uint64_t revision = 0;
    };

    void beginClosing() noexcept;
    static async::Task<Result<Mutation>> beginMutationAsync(
      Submission submission,
      CommandKind kind,
      std::uint64_t generation,
      library::WriteTransaction::Options options,
      std::string operation,
      compat::MoveOnlyFunction<Result<>(std::stop_token)> preTransaction = {});
    Result<std::shared_ptr<detail::LibraryMutationCommandRequest>> enqueueCommand(CommandKind kind,
                                                                                  std::uint64_t generation,
                                                                                  bool reentrant,
                                                                                  std::string_view operation);
    void releaseCommand(std::shared_ptr<detail::LibraryMutationCommandRequest> const& requestPtr) noexcept;
    bool beginTransaction() noexcept;
    Result<std::uint64_t> commitMutation(Mutation& mutation, LibraryChangeSet changeSet);
    async::Task<detail::LibraryPublicationTerminal> settleMutationAsync(std::uint64_t revision);
    [[noreturn]] void abortPostCommitSettlement(std::exception_ptr exceptionPtr) noexcept;
    void completeCommittedCommand(std::uint64_t revision) noexcept;
    void finishBackgroundTask(std::uint64_t generation) noexcept;
    static async::Task<void> finishMaintenanceAsync(
      std::weak_ptr<detail::LibraryMutationLifetimeState> lifetimeStatePtr,
      std::uint64_t generation);
    async::Task<bool> deliverControlAvailabilityAsync(LibraryAuthoringAvailability expected);
    void deliverControlAvailability(
      std::shared_ptr<detail::LibraryMutationControlDelivery> const& deliveryPtr) noexcept;
    void handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept;
    void finishPublication(detail::LibraryPublicationTerminal terminal,
                           std::uint64_t revision,
                           std::string libraryIdentity,
                           std::string replicaName);
    std::optional<PublicationDiagnosticContext> activePublicationDiagnosticContext() const noexcept;
    bool beginAvailabilityNotification(LibraryAuthoringAvailability const& expected) noexcept;
    void completeAvailabilityNotification() noexcept;
    void emitAvailability(LibraryAuthoringAvailability const& expected) noexcept;
    LibraryAuthoringAvailability availabilityLocked() const noexcept;
    bool submissionIsReentrantLocked() const noexcept;
    bool hasOutstandingCommandLocked(CommandKind kind) const noexcept;
    bool hasMaintenanceTransitionLocked() const noexcept;

    async::Executor& _callbackExecutor;
    library::WritableMusicLibrary _writableLibrary;
    library::MusicLibrary& _library;
    LibraryChanges& _changes;
    WriteTransactionFactory _writeTransactionFactory;
    std::uint64_t const _runtimeInstanceId;
    std::shared_ptr<detail::LibraryMutationLifetimeState> _lifetimeStatePtr;

    mutable std::mutex _stateMutex;
    std::condition_variable _stateChanged;
    std::deque<std::shared_ptr<detail::LibraryMutationCommandRequest>> _commandQueue;
    std::shared_ptr<detail::LibraryMutationCommandRequest> _activeCommandRequestPtr;
    std::shared_ptr<detail::LibraryMutationPublicationEvent> _activePublicationEventPtr;
    std::shared_ptr<detail::LibraryMutationControlDelivery> _activeControlDeliveryPtr;
    Lifecycle _lifecycle = Lifecycle::Open;
    ActivePhase _activePhase = ActivePhase::Idle;
    LibraryAuthoringState _state = LibraryAuthoringState::Available;
    std::uint64_t _lastCommittedRevision = 0;
    std::uint64_t _availableRevision = 0;
    std::uint64_t _maintenanceGeneration = 0;
    std::uint64_t _backgroundTaskGeneration = 0;
    std::optional<BackgroundTaskKind> _optBackgroundTaskKind;
    bool _changesSealed = false;
    std::optional<PublicationDiagnosticContext> _optActivePublicationDiagnosticContext;
    bool _availabilityNotificationInProgress = false;
    mutable async::Signal<LibraryAuthoringAvailability const&> _availabilityChanged;

    friend class Library;
    friend class LibraryJobs;
    friend class detail::LibraryMutationLifetimeState;
    friend class detail::LibraryMutationOwnerLease;
    friend struct detail::LibraryMutationCommandRequest;
    friend struct detail::LibraryMutationControlDelivery;
    friend struct detail::LibraryMutationPublicationEvent;
  };
} // namespace ao::rt
