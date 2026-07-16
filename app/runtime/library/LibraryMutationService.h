// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
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

      library::WriteTransaction& transaction() noexcept;
      Result<CommitInfo> commit(LibraryChangeSet changeSet);

    private:
      Mutation(LibraryMutationService& owner,
               std::unique_lock<std::mutex> writerLock,
               library::WriteTransaction transaction);

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

    private:
      MaintenanceGuard(LibraryMutationService& owner, std::uint64_t generation) noexcept;

      LibraryMutationService* _owner = nullptr;
      std::uint64_t _generation = 0;

      friend class LibraryMutationService;
    };

    struct AuthoringStart final
    {
      TrackAuthoringStatus status = TrackAuthoringStatus::Unavailable;
      std::optional<Mutation> optMutation{};
      std::vector<TrackId> missingTargetIds{};
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
    Subscription onAvailabilityChanged(
      std::move_only_function<void(LibraryAuthoringAvailability const&)> handler) const;
    Result<BoundTrackTargets> bindTrackTargets(std::span<TrackId const> trackIds) const;
    BoundTrackTargets advanceBoundTargets(BoundTrackTargets const& targets, std::uint64_t revision) const;

    Result<Mutation> beginInteractiveMutation();
    AuthoringStart beginAuthoringMutation(BoundTrackTargets const& targets);
    Result<MaintenanceGuard> beginMaintenance(LibraryMaintenanceKind kind);
    Result<Mutation> beginMaintenanceMutation(MaintenanceGuard const& guard);

  private:
    Result<std::unique_lock<std::mutex>> acquireWriter(LibraryAuthoringState requiredState, std::string_view operation);
    Result<CommitInfo> commit(Mutation& mutation, LibraryChangeSet changeSet);
    void finishMaintenance(std::uint64_t generation) noexcept;
    void handlePublication(std::uint64_t revision, std::exception_ptr failure);
    void emitAvailability(LibraryAuthoringAvailability const& expected);
    LibraryAuthoringAvailability availabilityLocked() const noexcept;

    async::Executor& _callbackExecutor;
    library::WritableMusicLibrary _writableLibrary;
    library::MusicLibrary& _library;
    LibraryChanges& _changes;
    std::uint64_t const _runtimeInstanceId;

    mutable std::mutex _stateMutex;
    std::mutex _writerMutex;
    std::condition_variable _publicationCompleted;
    LibraryAuthoringState _state = LibraryAuthoringState::Available;
    std::uint64_t _lastCommittedRevision = 0;
    std::uint64_t _availableRevision = 0;
    std::uint64_t _maintenanceGeneration = 0;
    LibraryMaintenanceKind _maintenanceKind = LibraryMaintenanceKind::None;
    bool _publicationInProgress = false;
    mutable Signal<LibraryAuthoringAvailability const&> _availabilityChanged;
  };
} // namespace ao::rt
