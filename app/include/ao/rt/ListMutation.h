// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::rt
{
  enum class LibraryListKind : std::uint8_t
  {
    Smart,
    Manual,
  };

  struct LibraryListDraft final
  {
    LibraryListKind kind = LibraryListKind::Smart;
    ListId parentId = kInvalidListId;
    ListId listId = kInvalidListId;
    std::string name{};
    std::string description{};
    std::string expression{};
    std::vector<TrackId> trackIds{};
  };

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
} // namespace ao::rt
