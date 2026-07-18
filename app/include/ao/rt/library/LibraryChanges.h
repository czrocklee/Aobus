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
    struct LibraryTaskProgressUpdated final
    {
      double fraction = 0.0;
      std::string message{};
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

    async::Subscription onChanged(std::move_only_function<void(LibraryChangeSet const&)> handler) const;
    async::Subscription onLibraryTaskCompleted(
      std::move_only_function<void(LibraryTaskCompleted const&)> handler) const;
    async::Subscription onLibraryTaskProgress(
      std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const;

  private:
    friend class LibraryMutationService;
    friend class LibraryTaskService;

    void publishFromCoordinator(LibraryChangeSet changeSet,
                                std::move_only_function<void(std::exception_ptr)> completion);
    void notifyLibraryTaskCompleted(LibraryTaskCompletionStatus status, std::size_t affectedCount = 0);
    void notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
