// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include "async/Task.h"
#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/Exporter.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::model
{
  struct ListDraft;
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
    LibraryMutationService(IControlExecutor& executor, library::MusicLibrary& library);
    ~LibraryMutationService();

    Result<UpdateTrackMetadataReply> updateMetadata(std::vector<TrackId> const& trackIds, MetadataPatch const& patch);
    Result<EditTrackTagsReply> editTags(std::vector<TrackId> const& trackIds,
                                        std::vector<std::string> const& tagsToAdd,
                                        std::vector<std::string> const& tagsToRemove);

    // Legacy synchronous/thread-spawning API
    ImportFilesReply importFiles(std::vector<std::filesystem::path> const& paths);

    // C++20 Async variants
    async::Task<void> importFilesAsync(async::Runtime& runtime, std::vector<std::filesystem::path> paths);
    async::Task<void> importLibraryAsync(async::Runtime& runtime, std::filesystem::path path);
    async::Task<void> exportLibraryAsync(async::Runtime& runtime, std::filesystem::path path, library::ExportMode mode);
    async::Task<std::vector<std::filesystem::path>> scanLibraryAsync(async::Runtime& runtime,
                                                                     std::filesystem::path dir);

    ListId createList(model::ListDraft const& draft);
    void updateList(model::ListDraft const& draft);
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
