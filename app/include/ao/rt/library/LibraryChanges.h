// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  struct ManualStoredRemoveRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};

    bool operator==(ManualStoredRemoveRange const&) const = default;
  };

  struct ManualTracksInsert final
  {
    std::size_t storedIndex = 0;
    std::vector<TrackId> trackIds{};

    bool operator==(ManualTracksInsert const&) const = default;
  };

  struct ManualTracksRemove final
  {
    std::vector<ManualStoredRemoveRange> removals{};

    bool operator==(ManualTracksRemove const&) const = default;
  };

  struct ManualTracksMove final
  {
    std::vector<ManualStoredRemoveRange> removals{};
    std::size_t insertionIndexAfterRemoval = 0;
    std::vector<TrackId> insertedTrackIds{};

    bool operator==(ManualTracksMove const&) const = default;
  };

  struct ManualTracksReset final
  {
    bool operator==(ManualTracksReset const&) const = default;
  };

  struct ManualListContentChange final
  {
    ListId listId = kInvalidListId;
    std::variant<ManualTracksInsert, ManualTracksRemove, ManualTracksMove, ManualTracksReset> operation{};

    bool operator==(ManualListContentChange const&) const = default;
  };

  struct LibraryChangeSet final
  {
    std::uint64_t libraryRevision = 0;
    bool libraryReset = false;
    std::vector<TrackId> tracksInserted{};
    std::vector<TrackId> tracksDeleted{};
    std::vector<TrackId> tracksMutated{};
    std::vector<ListId> listsUpserted{};
    std::vector<ListId> listsDeleted{};
    std::vector<ManualListContentChange> manualContentChanges{};

    bool operator==(LibraryChangeSet const&) const = default;
  };

  class LibraryTaskService;
  class LibraryMutationService;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    enum class LibraryTaskProgressKind : std::uint8_t
    {
      Scanning,
      Updating,
      Fingerprinting,
      IndexingAudioIdentity,
    };

    struct LibraryTaskProgressUpdated final
    {
      LibraryTaskProgressKind kind = LibraryTaskProgressKind::Scanning;
      double fraction = 0.0;
      std::string subject{};
    };

    enum class LibraryTaskCompletionStatus : std::uint8_t
    {
      Succeeded,
      CompletedWithIssues,
      Failed,
      Cancelled,
    };

    struct LibraryTaskCompleted final
    {
      LibraryTaskCompletionStatus status = LibraryTaskCompletionStatus::Succeeded;
      std::size_t affectedCount = 0;
    };

    LibraryChanges();
    LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision);
    ~LibraryChanges();

    LibraryChanges(LibraryChanges const&) = delete;
    LibraryChanges& operator=(LibraryChanges const&) = delete;
    LibraryChanges(LibraryChanges&&) = delete;
    LibraryChanges& operator=(LibraryChanges&&) = delete;

    // Publication runs in two phases (doc/spec/library/runtime/change-publication.md).
    //
    // Phase one delivers the revision to the single bound replica -- the one
    // consumer that keeps derived state the rest of the runtime reads from.
    // Applying a committed revision is a noexcept contract: failure is fatal,
    // not a recoverable publication result. At most one replica may be bound;
    // the returned handle unbinds. A new binding is rejected while publication
    // is active. Unbinding does not interrupt a replica already pinned for the
    // current delivery; it only prevents later deliveries.
    async::Subscription bindReplica(std::string replicaName,
                                    std::move_only_function<void(LibraryChangeSet const&) noexcept> apply) const;

    // Phase two announces an applied revision. Reaching an observer means
    // the replica applied the revision and the library is readable at it.
    // Observers are noexcept notifications.
    async::Subscription onChanged(std::move_only_function<void(LibraryChangeSet const&) noexcept> handler) const;
    async::Subscription onLibraryTaskCompleted(
      std::move_only_function<void(LibraryTaskCompleted const&) noexcept> handler) const;
    async::Subscription onLibraryTaskProgress(
      std::move_only_function<void(LibraryTaskProgressUpdated const&) noexcept> handler) const;

  private:
    friend class LibraryMutationService;
    friend class LibraryTaskService;

    void publishFromCoordinator(LibraryChangeSet changeSet,
                                std::move_only_function<void(std::exception_ptr)> completion);
    void notifyLibraryTaskCompleted(LibraryTaskCompletionStatus status, std::size_t affectedCount = 0);
    void notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress);

    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
