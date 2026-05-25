// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ao::rt
{
  enum class TrackGroupKey : std::uint8_t
  {
    None,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
  };

  enum class TrackSortField : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Title,
    Duration,
  };

  constexpr auto kTrackSortFieldCount = static_cast<std::size_t>(TrackSortField::Duration) + 1;

  struct TrackSortTerm final
  {
    TrackSortField field = TrackSortField::Title;
    bool ascending = true;

    bool operator==(TrackSortTerm const&) const = default;
  };

  enum class TrackField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,

    Year,
    DiscNumber,
    TotalDiscs,
    TrackNumber,
    TotalTracks,

    Duration,
    Tags,

    FilePath,
    Codec,
    SampleRate,
    Channels,
    BitDepth,
    Bitrate,
    FileSize,
    ModifiedTime,

    DisplayTrackNumber,
    TechnicalSummary,
    Quality,
  };

  constexpr auto kTrackFieldCount = static_cast<std::size_t>(TrackField::Quality) + 1;

  enum class TrackFieldCategory : std::uint8_t
  {
    Metadata,
    Tag,
    Technical,
    Synthetic,
  };

  enum class TrackFieldValueKind : std::uint8_t
  {
    Text,
    Number,
    Duration,
    TagList,
    FilePath,
    TechnicalText,
  };

  struct TrackFieldDefinition final
  {
    TrackField field;
    std::string_view id;
    std::string_view label;
    TrackFieldCategory category;
    TrackFieldValueKind valueKind;

    bool presentable = false;
    bool editable = false;
    bool sortable = false;
    bool groupable = false;
    bool synthetic = false;

    std::optional<TrackSortField> optSortField{};
    std::optional<TrackGroupKey> optGroupKey{};

    std::string_view filterExpressionVariable{};
  };

  std::span<TrackFieldDefinition const> trackFieldDefinitions();
  TrackFieldDefinition const* trackFieldDefinition(TrackField field);
  std::optional<TrackField> trackFieldFromId(std::string_view id);
  std::string_view trackFieldId(TrackField field);

  std::string_view trackFieldFilterExpressionVariable(TrackField field);
  bool trackFieldSupportsFilterExpression(TrackField field);
} // namespace ao::rt
