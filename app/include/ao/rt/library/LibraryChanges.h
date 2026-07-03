// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt
{
  class LibraryTasks;
  class LibraryWriter;

  class [[nodiscard]] LibraryChanges final
  {
  public:
    struct ListsMutated final
    {
      std::vector<ListId> upserted{};
      std::vector<ListId> deleted{};
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
    friend class LibraryTasks;
    friend class LibraryWriter;

    void notifyTracksMutated(std::vector<TrackId> trackIds);
    void notifyTrackCollectionChanged(std::vector<TrackId> inserted, std::vector<TrackId> deleted);
    void notifyListsMutated(std::vector<ListId> upserted, std::vector<ListId> deleted);
    void notifyLibraryTaskCompleted(std::size_t count);
    void notifyLibraryTaskProgress(LibraryTaskProgressUpdated progress);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
