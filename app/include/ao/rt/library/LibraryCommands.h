// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class LibraryWriteLane;
  struct MetadataPatch;
  struct UpdateTrackMetadataReply;

  // Asynchronous semantic mutation surface over the music library. Each
  // effective mutator commits and publishes through the runtime writeLane.
  // There is no caller-visible transaction scope, so a sequence of calls is a
  // sequence of independent commits rather than one atomic unit.
  class [[nodiscard]] LibraryCommands final
  {
  public:
    ~LibraryCommands();

    // Metadata and tag authoring requires runtime-created target evidence.
    // Every target is revalidated in the committing transaction; stale or
    // missing targets reject the complete command rather than applying a
    // subset. Storage and validation failures are returned as Result errors.
    // Preview methods run the same mutation path as their committing
    // counterparts, but return before commit and publish no change events.
    // Preview replies never include allocated ids; ids are only valid after a
    // successful committing call.
    async::Task<Result<TrackAuthoringResult<UpdateTrackMetadataReply>>> updateMetadata(BoundTrackTargets targets,
                                                                                       MetadataPatch patch);
    async::Task<Result<UpdateTrackMetadataReply>> previewUpdateMetadata(std::vector<TrackId> trackIds,
                                                                        MetadataPatch patch);
    async::Task<Result<TrackAuthoringResult<EditTrackTagsReply>>> editTags(BoundTrackTargets targets,
                                                                           std::vector<std::string> tagsToAdd,
                                                                           std::vector<std::string> tagsToRemove);
    async::Task<Result<EditTrackTagsReply>> previewEditTags(std::vector<TrackId> trackIds,
                                                            std::vector<std::string> tagsToAdd,
                                                            std::vector<std::string> tagsToRemove);
    async::Task<Result<TrackAuthoringResult<UpdateTrackPropertiesReply>>> updateProperties(BoundTrackTargets targets,
                                                                                           TrackPropertiesPatch patch);
    async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>> addTracksToList(ListId listId,
                                                                                    BoundTrackTargets targets);
    async::Task<Result<AddTracksToListReply>> previewAddTracksToList(ListId listId, std::vector<TrackId> trackIds);
    async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>> removeTracksFromList(
      ListId listId,
      BoundTrackTargets targets);
    async::Task<Result<RemoveTracksFromListReply>> previewRemoveTracksFromList(ListId listId,
                                                                               std::vector<TrackId> trackIds);

    // Returns an error when the draft is invalid, such as a malformed smart
    // filter or an invalid parent relationship.
    async::Task<Result<ListId>> createList(ListDraft draft);
    async::Task<Result<>> previewCreateList(ListDraft draft);
    // Returns NotFound if no list with draft.listId exists (e.g. a stale id), or
    // another error when the draft is invalid.
    async::Task<Result<UpdateListReply>> updateList(ListDraft draft);
    async::Task<Result<UpdateListReply>> previewUpdateList(ListDraft draft);
    async::Task<Result<AuthoringResult<MoveListOrderReply>>> moveListOrder(BoundListOrder order,
                                                                           std::vector<TrackId> selectedTrackIds,
                                                                           std::optional<TrackId> optBeforeTrackId);
    async::Task<Result<AuthoringResult<ResetListOrderReply>>> resetListOrder(BoundListOrder order);
    async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> forgetHiddenListOrder(BoundListOrder order);
    async::Task<Result<DeleteListReply>> deleteList(ListId listId, DeleteListOptions options = {});
    async::Task<Result<DeleteListReply>> previewDeleteList(ListId listId, DeleteListOptions options = {});
    async::Task<Result<DeleteListSubtreeReply>> deleteListAndDescendants(ListId listId, DeleteListOptions options = {});
    async::Task<Result<DeleteListSubtreeReply>> previewDeleteListAndDescendants(ListId listId,
                                                                                DeleteListOptions options = {});

    async::Task<Result<DeleteTrackReply>> deleteTrack(TrackId trackId);
    async::Task<Result<DeleteTrackReply>> previewDeleteTrack(TrackId trackId);
    // Imports one audio file under the music root. Recoverable failures include
    // missing/out-of-root files, unsupported or malformed media, and duplicate
    // manifest entries.
    async::Task<Result<CreateTrackReply>> createTrackFromFile(std::filesystem::path path);
    async::Task<Result<PreviewCreateTrackReply>> previewCreateTrackFromFile(std::filesystem::path path);

    LibraryCommands(LibraryCommands const&) = delete;
    LibraryCommands& operator=(LibraryCommands const&) = delete;
    LibraryCommands(LibraryCommands&&) = delete;
    LibraryCommands& operator=(LibraryCommands&&) = delete;

  private:
    LibraryCommands(library::MusicLibrary& library, LibraryWriteLane& writeLane, async::Runtime& asyncRuntime);

    struct Impl;
    std::shared_ptr<Impl> _implPtr;

    friend class Library;
  };
} // namespace ao::rt
