// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
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
  class LibraryChanges;
  struct LibraryChangeSet;

  class LibraryMutationService final
  {
  public:
    struct CommitInfo final
    {
      std::uint64_t libraryRevision = 0;
    };

    class [[nodiscard]] Mutation final
    {
    public:
      ~Mutation();

      Mutation(Mutation const&) = delete;
      Mutation& operator=(Mutation const&) = delete;
      Mutation(Mutation&& other) noexcept;
      Mutation& operator=(Mutation&& other) = delete;

      // An error result or exception terminalizes this mutation and releases
      // writer admission. Later apply() or commit() calls return InvalidState.
      template<
        typename Function,
        typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Function, library::WriteTransaction&>>>
        requires library::detail::IsResult<OperationResult>::value
      OperationResult apply(Function&& function)
      {
        if (_owner == nullptr || _terminal)
        {
          return makeError(Error::Code::InvalidState, "Library mutation is already terminal");
        }

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

      Result<CommitInfo> commit(LibraryChangeSet changeSet);

    private:
      Mutation(LibraryMutationService& owner,
               std::unique_lock<std::mutex> writerLock,
               library::WriteTransaction transaction);
      void abort() noexcept;

      LibraryMutationService* _owner = nullptr;
      std::unique_lock<std::mutex> _writerLock;
      library::WriteTransaction _transaction;
      bool _terminal = false;

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
    // already reached, so handlers are noexcept. Publication faults come from
    // revision admission or executor task admission, not from observers.
    // Handlers must defer owner destruction or runtime shutdown to a later
    // callback-executor turn instead of tearing down the active emitter.
    async::Subscription onAvailabilityChanged(
      std::move_only_function<void(LibraryAuthoringAvailability const&) noexcept> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::span<TrackId const> effectiveTrackIds) const;
    Result<BoundListOrder> bindListOrder(ListId listId, std::vector<TrackId>&& effectiveTrackIds) const;
    BoundTrackTargets advanceBoundTargets(BoundTrackTargets const& targets, std::uint64_t revision) const;

    Result<Mutation> beginInteractiveMutation();
    AuthoringStart beginAuthoringMutation(BoundTrackTargets const& targets);
    ListOrderAuthoringStart beginListOrderAuthoringMutation(BoundListOrder const& order);
    Result<MaintenanceGuard> beginMaintenance(LibraryMaintenanceKind kind);
    Result<Mutation> beginMaintenanceMutation(MaintenanceGuard const& guard);

  private:
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
    Result<CommitInfo> commit(Mutation& mutation, LibraryChangeSet changeSet);
    void dispatchMaintenanceFinish(std::uint64_t generation) noexcept;
    void handleFinalizationAdmissionFailure(std::exception_ptr exceptionPtr) noexcept;
    void finishMaintenance(std::uint64_t generation) noexcept;
    void finishPublication(std::uint64_t revision) noexcept;
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
    bool _availabilityNotificationInProgress = false;
    bool _closing = false;
    mutable async::Signal<LibraryAuthoringAvailability const&> _availabilityChanged;

    friend class Library;
    friend class LibraryTaskService;
  };
} // namespace ao::rt
