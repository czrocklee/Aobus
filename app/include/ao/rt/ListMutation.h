// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackMutation.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct LibraryListDraft final
  {
    ListId parentId = kInvalidListId;
    ListId listId = kInvalidListId;
    std::string name{};
    std::string description{};
    std::string expression{};
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
    std::vector<ListFieldChange> fieldChanges{};

    bool operator==(UpdateListReply const&) const = default;
  };

  struct MoveListOrderReply final
  {
    std::vector<TrackId> selectedTrackIds{};
    std::optional<TrackId> optBeforeTrackId{};

    bool operator==(MoveListOrderReply const&) const = default;
  };

  struct ResetListOrderReply final
  {
    std::size_t forgottenPositionCount = 0;

    bool operator==(ResetListOrderReply const&) const = default;
  };

  struct ForgetHiddenListOrderReply final
  {
    std::size_t forgottenPositionCount = 0;

    bool operator==(ForgetHiddenListOrderReply const&) const = default;
  };

  struct AddTracksToListReply final
  {
    ListId listId{};
    std::string listName{};
    std::string tag{};
    std::vector<TrackId> targetTrackIds{};
    EditTrackTagsReply tagEdit{};

    bool operator==(AddTracksToListReply const&) const = default;
  };

  struct RemoveTracksFromListReply final
  {
    ListId listId{};
    std::string listName{};
    std::string tag{};
    std::vector<TrackId> targetTrackIds{};
    EditTrackTagsReply tagEdit{};
    std::vector<TrackId> forgottenPositionTrackIds{};

    bool operator==(RemoveTracksFromListReply const&) const = default;
  };

  struct DeleteListReply final
  {
    ListId listId{};
    std::string name{};
    std::size_t orderTrackIdCount = 0;
    struct TagReference final
    {
      ListId listId{};
      std::string name{};

      bool operator==(TagReference const&) const = default;
    };

    struct TagImpact final
    {
      std::string tag{};
      std::size_t taggedTrackCount = 0;
      std::size_t removedFromTrackCount = 0;
      std::vector<TagReference> otherListReferences{};

      bool operator==(TagImpact const&) const = default;
    };

    std::optional<TagImpact> optTagImpact{};

    bool operator==(DeleteListReply const&) const = default;
  };

  struct DeleteListOptions final
  {
    bool removeWritableTagFromTracks = false;
  };

  struct DeleteListSubtreeReply final
  {
    ListId rootListId{};
    std::vector<DeleteListReply> deletedLists{};

    bool operator==(DeleteListSubtreeReply const&) const = default;
  };
} // namespace ao::rt
