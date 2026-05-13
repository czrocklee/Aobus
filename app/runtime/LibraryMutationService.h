// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include <filesystem>
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

  class LibraryMutationService final
  {
  public:
    LibraryMutationService(IControlExecutor& executor, ao::library::MusicLibrary& library);
    ~LibraryMutationService();

    ao::Result<UpdateTrackMetadataReply> updateMetadata(std::vector<TrackId> const& trackIds,
                                                        MetadataPatch const& patch);
    ao::Result<EditTrackTagsReply> editTags(std::vector<TrackId> const& trackIds,
                                            std::vector<std::string> const& tagsToAdd,
                                            std::vector<std::string> const& tagsToRemove);
    ImportFilesReply importFiles(std::vector<std::filesystem::path> const& paths);

    ListId createList(ao::model::ListDraft const& draft);
    void updateList(ao::model::ListDraft const& draft);
    void deleteList(ListId listId);

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
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
