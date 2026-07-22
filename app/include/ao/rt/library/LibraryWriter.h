// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryMutationService;
  struct MetadataPatch;
  struct UpdateTrackMetadataReply;

  // Synchronous semantic mutation surface over the music library. Each
  // effective mutator commits and publishes through the runtime mutationService.
  // There is no caller-visible transaction scope, so a sequence of calls is a
  // sequence of independent commits rather than one atomic unit.
  class [[nodiscard]] LibraryWriter final
  {
  public:
    using MetadataAuthoringResult = TrackAuthoringResult<UpdateTrackMetadataReply>;
    using TagAuthoringResult = TrackAuthoringResult<EditTrackTagsReply>;

    using ListKind = LibraryListKind;
    using ListDraft = LibraryListDraft;

    ~LibraryWriter();

    // Metadata and tag authoring requires runtime-created target evidence.
    // Every target is revalidated in the committing transaction; stale or
    // missing targets reject the complete command rather than applying a
    // subset. Storage and validation failures are returned as Result errors.
    // Preview methods run the same mutation path as their committing
    // counterparts, but return before commit and publish no change events.
    // Preview replies never include allocated ids; ids are only valid after a
    // successful committing call.
    Result<MetadataAuthoringResult> updateMetadata(BoundTrackTargets const& targets, MetadataPatch const& patch);
    Result<UpdateTrackMetadataReply> previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                           MetadataPatch const& patch);
    Result<TagAuthoringResult> editTags(BoundTrackTargets const& targets,
                                        std::span<std::string const> tagsToAdd,
                                        std::span<std::string const> tagsToRemove);
    Result<EditTrackTagsReply> previewEditTags(std::span<TrackId const> trackIds,
                                               std::span<std::string const> tagsToAdd,
                                               std::span<std::string const> tagsToRemove);

    // Returns an error when the draft is invalid, such as a malformed smart
    // filter or an invalid parent relationship.
    Result<ListId> createList(ListDraft const& draft);
    Result<> previewCreateList(ListDraft const& draft);
    // Returns NotFound if no list with draft.listId exists (e.g. a stale id), or
    // another error when the draft is invalid.
    Result<UpdateListReply> updateList(ListDraft const& draft);
    Result<UpdateListReply> previewUpdateList(ListDraft const& draft);
    Result<InsertManualListTracksReply> insertManualListTracks(ListId listId,
                                                               std::size_t insertionIndex,
                                                               std::span<TrackId const> trackIds);
    Result<InsertManualListTracksReply> previewInsertManualListTracks(ListId listId,
                                                                      std::size_t insertionIndex,
                                                                      std::span<TrackId const> trackIds);
    Result<RemoveManualListTracksReply> removeManualListTracks(ListId listId, std::span<TrackId const> trackIds);
    Result<RemoveManualListTracksReply> previewRemoveManualListTracks(ListId listId, std::span<TrackId const> trackIds);
    Result<MoveManualListTracksReply> moveManualListTracks(ListId listId,
                                                           std::span<TrackId const> trackIds,
                                                           std::size_t insertionIndexAfterRemoval);
    Result<MoveManualListTracksReply> previewMoveManualListTracks(ListId listId,
                                                                  std::span<TrackId const> trackIds,
                                                                  std::size_t insertionIndexAfterRemoval);
    Result<DeleteListReply> deleteList(ListId listId);
    Result<DeleteListReply> previewDeleteList(ListId listId);

    Result<DeleteTrackReply> deleteTrack(TrackId trackId);
    Result<DeleteTrackReply> previewDeleteTrack(TrackId trackId);
    // Imports one audio file under the music root. Recoverable failures include
    // missing/out-of-root files, unsupported or malformed media, and duplicate
    // manifest entries.
    Result<CreateTrackReply> createTrackFromFile(std::filesystem::path const& path);
    Result<PreviewCreateTrackReply> previewCreateTrackFromFile(std::filesystem::path const& path);

    LibraryWriter(LibraryWriter const&) = delete;
    LibraryWriter& operator=(LibraryWriter const&) = delete;
    LibraryWriter(LibraryWriter&&) = delete;
    LibraryWriter& operator=(LibraryWriter&&) = delete;

  private:
    LibraryWriter(library::MusicLibrary& library, LibraryMutationService& mutationService);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class Library;
  };
} // namespace ao::rt
