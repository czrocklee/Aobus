// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>

#include <bitset>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct CreditReplacement final
  {
    std::bitset<library::kCreditKindCount> kinds{};
    std::vector<library::Credit> entries{};

    bool operator==(CreditReplacement const&) const = default;
  };

  struct MetadataPatch final
  {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optWork{};
    std::optional<std::string> optMovement{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTrackTotal{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optDiscTotal{};
    std::optional<std::uint16_t> optMovementNumber{};
    std::optional<std::uint16_t> optMovementTotal{};

    // An absent optional leaves the field unchanged; the all-zero sentinel
    // clears the stored date; any present value replaces it at its own
    // precision. Exact value equality includes the stored precision.
    std::optional<library::RecordingDate> optRecordingDate{};
    // An absent patch preserves credits. A present nonempty mask replaces
    // selected segments completely, preserving every unselected segment.
    // Empty entries clear the selected scope without changing RecordingDate.
    std::optional<CreditReplacement> optCredits{};

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

  /** One atomic metadata-and-tag edit for a bound Track selection. */
  struct TrackPropertiesPatch final
  {
    MetadataPatch metadata{};
    std::vector<std::string> tagsToAdd{};
    std::vector<std::string> tagsToRemove{};
  };

  struct UpdateTrackPropertiesReply final
  {
    UpdateTrackMetadataReply metadata{};
    EditTrackTagsReply tags{};

    bool operator==(UpdateTrackPropertiesReply const&) const = default;
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
