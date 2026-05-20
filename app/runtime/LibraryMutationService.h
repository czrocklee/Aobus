// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include "ao/Error.h"
#include "ao/Type.h"
#include "async/Task.h"
#include "runtime/LibraryExporter.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class IControlExecutor;

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
      std::string expression{};      // Only used for Smart lists
      std::vector<TrackId> trackIds; // Only used for Manual lists
    };

    LibraryMutationService(async::Runtime& asyncRuntime, library::MusicLibrary& library);
    ~LibraryMutationService();

    Result<UpdateTrackMetadataReply> updateMetadata(std::vector<TrackId> const& trackIds, MetadataPatch const& patch);
    Result<EditTrackTagsReply> editTags(std::vector<TrackId> const& trackIds,
                                        std::vector<std::string> const& tagsToAdd,
                                        std::vector<std::string> const& tagsToRemove);

    // Legacy synchronous/thread-spawning API
    ImportFilesReply importFiles(std::vector<std::filesystem::path> const& paths);

    // C++20 Async variants
    async::Task<void> importFilesAsync(std::vector<std::filesystem::path> paths);
    async::Task<void> importLibraryAsync(std::filesystem::path path);
    async::Task<void> exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode);
    async::Task<std::vector<std::filesystem::path>> scanLibraryAsync(std::filesystem::path dir);

    ListId createList(ListDraft const& draft);
    void updateList(ListDraft const& draft);
    void deleteList(ListId listId);

    void notifyTracksMutated(std::vector<TrackId> const& trackIds);
    void notifyListsMutated(std::vector<ListId> const& upserted, std::vector<ListId> const& deleted);

    struct ListsMutated final
    {
      std::vector<ListId> upserted{};
      std::vector<ListId> deleted{};
    };

    struct ImportProgressUpdated final
    {
      double fraction = 0.0;
      std::string message{};
    };

    Subscription onTracksMutated(std::move_only_function<void(std::vector<TrackId> const&)> handler);
    Subscription onListsMutated(std::move_only_function<void(ListsMutated const&)> handler);
    Subscription onImportCompleted(std::move_only_function<void(std::size_t)> handler);
    Subscription onImportProgress(std::move_only_function<void(ImportProgressUpdated const&)> handler);

    LibraryMutationService(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService const&) = delete;
    LibraryMutationService(LibraryMutationService&&) = delete;
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
