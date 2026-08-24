// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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

    // Completion acknowledgement has no recoverable branch after commit.
    void requireMatchingPublicationCompletion(bool publicationInProgress,
                                              std::uint64_t revision,
                                              std::uint64_t committedRevision,
                                              std::string_view libraryIdentity,
                                              std::string_view replicaName);
  } // namespace detail

  class LibraryMutationService final
  {
  public:
    class [[nodiscard]] Mutation final
    {
    public:
      ~Mutation();

      Mutation(Mutation const&) = delete;
      Mutation& operator=(Mutation const&) = delete;
      Mutation(Mutation&& other) noexcept;
      Mutation& operator=(Mutation&& other) = delete;

      // An error result or exception terminalizes this mutation and releases
      // writer admission. Later operations violate the terminal-state contract.
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
      auto execute(Operation&& operation, std::string_view operationName = "Library mutation")
      {
        using Value = detail::OperationResultTraits<OperationResult>::ValueType;
        using Execution = MutationExecution<Value>;

        static_assert(std::is_nothrow_move_constructible_v<Value>);
        static_assert(std::is_nothrow_move_constructible_v<LibraryChangeSet>);
        AO_INVARIANT(_owner != nullptr && !_terminal, "Library mutation is already terminal");
        AO_INVARIANT(_executeEligible, "Library mutation execute must own the first write operation");
        _executeEligible = false;

        try
        {
          auto outcomeRes = _transaction.apply(std::forward<Operation>(operation));

          if (!outcomeRes)
          {
            auto error = std::move(outcomeRes.error());
            abort();
            return Result<Execution>{std::unexpected{std::move(error)}};
          }

          if (auto* unchanged = std::get_if<Unchanged<Value>>(&*outcomeRes); unchanged != nullptr)
          {
            auto execution = Execution{.value = std::move(unchanged->value), .optCommittedRevision = std::nullopt};
            abort();
            return Result<Execution>{std::move(execution)};
          }

          auto changed = std::get<Changed<Value>>(std::move(*outcomeRes));
          auto execution = Execution{.value = std::move(changed.value), .optCommittedRevision = std::nullopt};
          auto commitRes = _owner->commitMutation(*this, std::move(changed.changeSet));

          if (!commitRes)
          {
            auto error = std::move(commitRes.error());
            error.message = std::format("{} commit failed: {}", operationName, error.message);
            return Result<Execution>{std::unexpected{std::move(error)}};
          }

          execution.optCommittedRevision = *commitRes;
          return Result<Execution>{std::move(execution)};
        }
        catch (...)
        {
          abort();
          throw;
        }
      }

      void abort() noexcept;

    private:
      Mutation(LibraryMutationService& owner,
               std::unique_lock<std::mutex> writerLock,
               library::WriteTransaction transaction);
      LibraryMutationService* _owner = nullptr;
      std::unique_lock<std::mutex> _writerLock;
      library::WriteTransaction _transaction;
      bool _terminal = false;
      bool _executeEligible = true;

      friend class LibraryMutationService;
    };

    class [[nodiscard]] MaintenanceGuard final
    {
    public:
      ~MaintenanceGuard();

      MaintenanceGuard(MaintenanceGuard const&) = delete;
      MaintenanceGuard& operator=(MaintenanceGuard const&) = delete;
      MaintenanceGuard(MaintenanceGuard&& other) noexcept;
      MaintenanceGuard& operator=(MaintenanceGuard&&) = delete;

      void finish() noexcept;

    private:
      struct LifetimeState;

      MaintenanceGuard(std::weak_ptr<LifetimeState> lifetimeStatePtr, std::uint64_t generation) noexcept;

      std::weak_ptr<LifetimeState> _lifetimeStatePtr;
      std::uint64_t _generation = 0;

      friend class LibraryMutationService;
    };

    struct AuthoringStart final
    {
      TrackAuthoringStatus status = TrackAuthoringStatus::Unavailable;
      std::optional<Mutation> optMutation{};
    };

    struct ListOrderAuthoringStart final
    {
      ListOrderAuthoringStatus status = ListOrderAuthoringStatus::Unavailable;
      std::optional<Mutation> optMutation{};
    };

    LibraryMutationService(async::Executor& callbackExecutor,
                           library::WritableMusicLibrary writableLibrary,
                           LibraryChanges& changes);
    ~LibraryMutationService();

    LibraryMutationService(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService const&) = delete;
    LibraryMutationService(LibraryMutationService&&) = delete;
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

    LibraryAuthoringAvailability availability() const;
    // Availability is a notification: it reports a state the coordinator has
    // already reached. The owning Signal boundary diagnoses and aborts an
    // escaping observer exception.
    // Handlers must defer owner destruction or runtime shutdown to a later
    // callback-executor turn instead of tearing down the active emitter.
    async::Subscription onAvailabilityChanged(
      compat::MoveOnlyFunction<void(LibraryAuthoringAvailability const&)> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::span<TrackId const> effectiveTrackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::vector<TrackId>&& effectiveTrackIds) const;
    BoundTrackTargets advanceBoundTargets(BoundTrackTargets const& targets, std::uint64_t revision) const;

    Result<Mutation> beginInteractiveMutation(library::WriteTransaction::Options options = {});
    AuthoringStart beginAuthoringMutation(BoundTrackTargets const& targets);
    ListOrderAuthoringStart beginListOrderAuthoringMutation(BoundListOrder const& order);
    Result<MaintenanceGuard> beginMaintenance(LibraryMaintenanceKind kind);
    Result<Mutation> beginMaintenanceMutation(MaintenanceGuard const& guard);

  private:
    struct PublicationDiagnosticContext final
    {
      std::string libraryIdentity;
      std::string replicaName;
      std::uint64_t revision = 0;
    };

    // Submission return and publication completion rendezvous in either order;
    // writer admission reopens only after both have completed.
    class PublicationBarrier final
    {
    public:
      constexpr void beginSubmission(bool const fromOwner) noexcept
      {
        _publicationInProgress = true;
        _submissionInProgress = true;
        _submissionFromOwner = fromOwner;
      }

      constexpr void completeSubmission() noexcept
      {
        _submissionInProgress = false;
        _submissionFromOwner = false;
      }

      constexpr void completePublication() noexcept { _publicationInProgress = false; }
      constexpr void retire() noexcept { _publicationInProgress = false; }

      constexpr bool blocksWriter() const noexcept { return _publicationInProgress || _submissionInProgress; }

      constexpr bool publicationInProgress() const noexcept { return _publicationInProgress; }
      constexpr bool submissionInProgress() const noexcept { return _submissionInProgress; }
      constexpr bool ownerSubmissionInProgress() const noexcept
      {
        return _submissionInProgress && _submissionFromOwner;
      }

    private:
      bool _publicationInProgress = false;
      bool _submissionInProgress = false;
      bool _submissionFromOwner = false;
    };

    void beginClosing() noexcept;
    Result<std::unique_lock<std::mutex>> acquireWriter(LibraryAuthoringState requiredState, std::string_view operation);
    Result<std::uint64_t> commitMutation(Mutation& mutation, LibraryChangeSet changeSet);
    void dispatchMaintenanceFinish(std::uint64_t generation) noexcept;
    void handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept;
    void finishMaintenance(std::uint64_t generation) noexcept;
    void finishPublication(std::uint64_t revision, std::string libraryIdentity, std::string replicaName);
    std::shared_ptr<PublicationDiagnosticContext const> activePublicationDiagnosticContext() const noexcept;
    bool beginAvailabilityNotification(LibraryAuthoringAvailability const& expected) noexcept;
    void completeAvailabilityNotification() noexcept;
    void emitAvailability(LibraryAuthoringAvailability const& expected) noexcept;
    void emitAvailability(LibraryAuthoringAvailability const& expected,
                          std::unique_lock<std::mutex>& writerLock) noexcept;
    bool writerAdmissionBlockedLocked() const noexcept;
    LibraryAuthoringAvailability availabilityLocked() const noexcept;

    async::Executor& _callbackExecutor;
    library::WritableMusicLibrary _writableLibrary;
    library::MusicLibrary& _library;
    LibraryChanges& _changes;
    std::uint64_t const _runtimeInstanceId;
    std::shared_ptr<MaintenanceGuard::LifetimeState> _lifetimeStatePtr;

    mutable std::mutex _stateMutex;
    std::mutex _writerMutex;
    std::condition_variable _writerAdmissionChanged;
    LibraryAuthoringState _state = LibraryAuthoringState::Available;
    std::uint64_t _lastCommittedRevision = 0;
    std::uint64_t _availableRevision = 0;
    std::uint64_t _maintenanceGeneration = 0;
    LibraryMaintenanceKind _maintenanceKind = LibraryMaintenanceKind::None;
    PublicationBarrier _publicationBarrier;
    std::shared_ptr<PublicationDiagnosticContext const> _activePublicationDiagnosticContextPtr;
    bool _availabilityNotificationInProgress = false;
    bool _closing = false;
    mutable async::Signal<LibraryAuthoringAvailability const&> _availabilityChanged;

    friend class Library;
    friend class LibraryTaskService;
  };
} // namespace ao::rt
