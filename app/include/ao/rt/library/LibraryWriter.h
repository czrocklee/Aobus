// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackMutation.h>

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
  class LibraryChanges;
  struct MetadataPatch;
  struct UpdateTrackMetadataReply;

  struct ListFieldChange final
  {
    std::string field{};
    std::string oldValue{};
    std::string newValue{};

    bool operator==(ListFieldChange const&) const = default;
  };

  struct UpdateListReply final
  {
    bool changed = false;
    bool trackOrderChanged = false;
    std::vector<ListFieldChange> fieldChanges{};
    std::vector<TrackId> addedTrackIds{};
    std::vector<TrackId> removedTrackIds{};

    bool operator==(UpdateListReply const&) const = default;
  };

  struct InsertManualListTracksReply final
  {
    bool changed = false;
    std::size_t insertionIndex = 0;
    std::vector<TrackId> insertedTrackIds{};
    std::vector<TrackId> duplicateRequest{};
    std::vector<TrackId> alreadyPresent{};
    std::vector<TrackId> missingTrack{};

    bool operator==(InsertManualListTracksReply const&) const = default;
  };

  struct RemoveManualListTracksReply final
  {
    bool changed = false;
    std::vector<TrackId> removedTrackIds{};
    std::vector<TrackId> duplicateRequest{};
    std::vector<TrackId> notPresent{};

    bool operator==(RemoveManualListTracksReply const&) const = default;
  };

  struct MoveManualListTracksReply final
  {
    bool changed = false;
    std::size_t insertionIndexAfterRemoval = 0;
    std::vector<TrackId> selectedTrackIds{};
    std::vector<TrackId> duplicateRequest{};
    std::vector<TrackId> notPresent{};

    bool operator==(MoveManualListTracksReply const&) const = default;
  };

  struct DeleteListReply final
  {
    ListId listId{};
    std::string name{};
    std::string kind{};
    std::uint64_t trackCount = 0;

    bool operator==(DeleteListReply const&) const = default;
  };

  // Synchronous mutation surface over the music library. Each mutator opens,
  // commits and closes its own write transaction before returning, then
  // publishes the corresponding change event. There is no caller-visible
  // transaction scope, so a sequence of calls is a sequence of independent
  // commits rather than one atomic unit; multi-step atomic writes are out of
  // scope for this API. A mutator that detects no effective change (a stale id,
  // a no-op patch) commits nothing and reports the empty result for its return
  // type (see each signature below).
  class [[nodiscard]] LibraryWriter final
  {
  public:
    enum class ListKind : std::uint8_t
    {
      Smart,
      Manual
    };

    struct ListDraft final
    {
      ListKind kind = ListKind::Smart;
      ListId parentId = kInvalidListId;
      ListId listId = kInvalidListId;
      std::string name{};
      std::string description{};
      std::string expression{};
      std::vector<TrackId> trackIds{};
    };

    LibraryWriter(library::MusicLibrary& library, LibraryChanges& changes);
    ~LibraryWriter();

    // Applies a metadata patch to each resolved track. Tracks that do not exist
    // or whose values are unchanged are skipped; the reply's mutatedIds lists
    // only the tracks that genuinely changed (empty = nothing applied). Storage
    // and serialization failures are returned as Result errors.
    // Preview methods run the same mutation path as their committing
    // counterparts, but return before commit and publish no change events.
    // Preview replies never include allocated ids; ids are only valid after a
    // successful committing call.
    Result<UpdateTrackMetadataReply> updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch);
    Result<UpdateTrackMetadataReply> previewUpdateMetadata(std::span<TrackId const> trackIds,
                                                           MetadataPatch const& patch);
    Result<EditTrackTagsReply> editTags(std::span<TrackId const> trackIds,
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
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
