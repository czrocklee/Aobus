// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackMutation.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
    // only the tracks that genuinely changed (empty = nothing applied).
    UpdateTrackMetadataReply updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch);
    EditTrackTagsReply editTags(std::span<TrackId const> trackIds,
                                std::span<std::string const> tagsToAdd,
                                std::span<std::string const> tagsToRemove);

    ListId createList(ListDraft const& draft);
    // Returns false if no list with draft.listId exists (e.g. a stale id).
    bool updateList(ListDraft const& draft);
    // Returns false if no list with listId exists.
    bool deleteList(ListId listId);

    // Returns false if no track with trackId exists.
    bool deleteTrack(TrackId trackId);
    // Returns nullopt if the file cannot be opened or parsed as a track.
    std::optional<TrackId> createTrackFromFile(std::filesystem::path const& path);

    LibraryWriter(LibraryWriter const&) = delete;
    LibraryWriter& operator=(LibraryWriter const&) = delete;
    LibraryWriter(LibraryWriter&&) = delete;
    LibraryWriter& operator=(LibraryWriter&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
