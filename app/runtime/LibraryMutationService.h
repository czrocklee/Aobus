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

namespace ao::app
{
  class EventBus;
  class IControlExecutor;

  class LibraryMutationService final
  {
  public:
    LibraryMutationService(EventBus& events, IControlExecutor& executor, ao::library::MusicLibrary& library);
    ~LibraryMutationService();

    ao::Result<UpdateTrackMetadataReply> updateMetadata(std::vector<ao::TrackId> const& trackIds,
                                                        MetadataPatch const& patch);
    ao::Result<EditTrackTagsReply> editTags(std::vector<ao::TrackId> const& trackIds,
                                            std::vector<std::string> const& tagsToAdd,
                                            std::vector<std::string> const& tagsToRemove);
    ImportFilesReply importFiles(std::vector<std::filesystem::path> const& paths);

    LibraryMutationService(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
