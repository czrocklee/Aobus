// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>

#include <cstddef>
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
  class LibraryWriter;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    struct LibraryTaskProgressUpdated final
    {
      double fraction = 0.0;
      std::string message{};
    };

    LibraryChanges();
    LibraryChanges(async::Executor& callbackExecutor, std::uint64_t lastPublishedRevision);
    ~LibraryChanges();

    LibraryChanges(LibraryChanges const&) = delete;
    LibraryChanges& operator=(LibraryChanges const&) = delete;
    LibraryChanges(LibraryChanges&&) = delete;
    LibraryChanges& operator=(LibraryChanges&&) = delete;

    Subscription onChanged(std::move_only_function<void(LibraryChangeSet const&)> handler) const;
    Subscription onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler) const;
    Subscription onLibraryTaskProgress(std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const;

    void publish(LibraryChangeSet changeSet);

  private:
    friend class LibraryTaskService;
    friend class LibraryWriter;

    void notifyLibraryTaskCompleted(std::size_t count);
    void notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
