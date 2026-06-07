// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include "async/Task.h"
#include <ao/Error.h>
#include <ao/Type.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
  struct ScanPlan;
}

namespace ao::rt
{
  class IControlExecutor;
  struct MetadataPatch;
  struct UpdateTrackMetadataReply;
  enum class ExportMode : std::uint8_t;

  namespace async
  {
    class Runtime;
  }

  class LibraryMutationService final
  {
  public:
    enum class ListKind : std::uint8_t
    {
      Smart, // Expression-based filtered list
      Manual // Explicit TrackId list
    };

    /**
     * ListDraft - Plain data transfer object for list creation.
     */
    struct ListDraft final
    {
      ListKind kind = ListKind::Smart;
      ListId parentId = kInvalidListId;
      ListId listId = kInvalidListId; // 0 = create, non-zero = update
      std::string name{};
      std::string description{};
      std::string expression{};        // Only used for Smart lists
      std::vector<TrackId> trackIds{}; // Only used for Manual lists
    };

    LibraryMutationService(async::Runtime& asyncRuntime, library::MusicLibrary& library);
    ~LibraryMutationService();

    Result<UpdateTrackMetadataReply> updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch);
    Result<EditTrackTagsReply> editTags(std::span<TrackId const> trackIds,
                                        std::span<std::string const> tagsToAdd,
                                        std::span<std::string const> tagsToRemove);

    // C++20 Async variants
    async::Task<void> importLibraryAsync(std::filesystem::path path);
    async::Task<void> exportLibraryAsync(std::filesystem::path path, ExportMode mode);
    async::Task<library::ScanPlan> buildScanPlanAsync();
    async::Task<void> applyScanPlanAsync(library::ScanPlan plan);

    ListId createList(ListDraft const& draft);
    void updateList(ListDraft const& draft);
    void deleteList(ListId listId);

    void notifyTracksMutated(std::vector<TrackId> trackIds);
    void notifyListsMutated(std::vector<ListId> upserted, std::vector<ListId> deleted);
    void notifyLibraryTaskCompleted(std::size_t count);

    struct ListsMutated final
    {
      std::vector<ListId> upserted{};
      std::vector<ListId> deleted{};
    };

    struct LibraryTaskProgressUpdated final
    {
      double fraction = 0.0;
      std::string message{};
    };

    Subscription onTracksMutated(std::move_only_function<void(std::vector<TrackId> const&)> handler);
    Subscription onListsMutated(std::move_only_function<void(ListsMutated const&)> handler);
    Subscription onLibraryTaskCompleted(std::move_only_function<void(std::size_t)> handler);
    Subscription onLibraryTaskProgress(std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler);

    LibraryMutationService(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService const&) = delete;
    LibraryMutationService(LibraryMutationService&&) = delete;
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
}
