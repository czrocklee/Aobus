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

  class LibraryTaskService;
  class LibraryWriter;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    struct ListsMutated final
    {
      std::vector<ListId> upserted{};
      std::vector<ListId> deleted{};
      std::vector<ManualListContentChange> manualContentChanges{};
    };

    struct TrackCollectionChanged final
    {
      std::vector<TrackId> inserted{};
      std::vector<TrackId> deleted{};
    };

    struct LibraryTaskProgressUpdated final
    {
      double fraction = 0.0;
      std::string message{};
    };

    LibraryChanges();
    ~LibraryChanges();

    LibraryChanges(LibraryChanges const&) = delete;
    LibraryChanges& operator=(LibraryChanges const&) = delete;
    LibraryChanges(LibraryChanges&&) = delete;
    LibraryChanges& operator=(LibraryChanges&&) = delete;

    Subscription onTracksMutated(std::move_only_function<void(std::vector<TrackId> const&)> handler) const;
    Subscription onTrackCollectionChanged(std::move_only_function<void(TrackCollectionChanged const&)> handler) const;
    Subscription onListsMutated(std::move_only_function<void(ListsMutated const&)> handler) const;
    Subscription onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler) const;
    Subscription onLibraryTaskProgress(std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const;

  private:
    friend class LibraryTaskService;
    friend class LibraryWriter;

    void notifyTracksMutated(std::vector<TrackId> trackIds);
    void notifyTrackCollectionChanged(std::vector<TrackId> inserted, std::vector<TrackId> deleted);
    void notifyListsMutated(std::vector<ListId> upserted,
                            std::vector<ListId> deleted,
                            std::vector<ManualListContentChange> manualContentChanges = {});
    void notifyLibraryTaskCompleted(std::size_t count);
    void notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
