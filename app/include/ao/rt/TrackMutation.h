// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct MetadataPatch final
  {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optConductor{};
    std::optional<std::string> optEnsemble{};
    std::optional<std::string> optWork{};
    std::optional<std::string> optMovement{};
    std::optional<std::string> optSoloist{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTrackTotal{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optDiscTotal{};
    std::optional<std::uint16_t> optMovementNumber{};
    std::optional<std::uint16_t> optMovementTotal{};

    std::map<std::string, std::optional<std::string>> customUpdates{};
  };

  struct TrackFieldChange final
  {
    std::string field;
    std::string oldValue;
    std::string newValue;

    bool operator==(TrackFieldChange const&) const = default;
  };

  struct TrackChangeRecord final
  {
    TrackId trackId{};
    std::vector<TrackFieldChange> fields;

    bool operator==(TrackChangeRecord const&) const = default;
  };

  struct TrackTagsChange final
  {
    TrackId trackId{};
    std::vector<std::string> addedTags;
    std::vector<std::string> removedTags;

    bool operator==(TrackTagsChange const&) const = default;
  };

  struct UpdateTrackMetadataReply final
  {
    std::vector<TrackChangeRecord> changes{};

    bool operator==(UpdateTrackMetadataReply const&) const = default;
  };

  struct EditTrackTagsReply final
  {
    std::vector<TrackTagsChange> changes{};

    bool operator==(EditTrackTagsReply const&) const = default;
  };

  struct CreateTrackReply final
  {
    TrackId trackId{};
    std::string uri{};
    std::string title{};
    std::string artist{};

    bool operator==(CreateTrackReply const&) const = default;
  };

  struct PreviewCreateTrackReply final
  {
    std::string uri{};
    std::string title{};
    std::string artist{};

    bool operator==(PreviewCreateTrackReply const&) const = default;
  };

  struct DeleteTrackReply final
  {
    TrackId trackId{};
    std::string uri{};
    std::string title{};
    std::vector<ListId> removedFromListIds{};

    bool operator==(DeleteTrackReply const&) const = default;
  };
} // namespace ao::rt
