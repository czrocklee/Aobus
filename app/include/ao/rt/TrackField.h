// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Field.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
    Conductor,
    Ensemble,
    Work,
    Year,
  };

  constexpr auto kTrackGroupKeyCount = static_cast<std::size_t>(TrackGroupKey::Year) + 1;

  enum class TrackSortField : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Conductor,
    Ensemble,
    Work,
    Movement,
    Soloist,
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
    Conductor,
    Ensemble,
    Work,
    Movement,
    Soloist,

    Year,
    DiscNumber,
    DiscTotal,
    TrackNumber,
    TrackTotal,
    MovementNumber,
    MovementTotal,

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

  template<typename Value>
  Value& trackFieldArrayAt(std::array<Value, kTrackFieldCount>& values, TrackField const field)
  {
    return values.at(static_cast<std::size_t>(field));
  }

  template<typename Value>
  Value const& trackFieldArrayAt(std::array<Value, kTrackFieldCount> const& values, TrackField const field)
  {
    return values.at(static_cast<std::size_t>(field));
  }

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
    bool valueCompletion = false;

    std::optional<TrackSortField> optSortField{};
    std::optional<TrackGroupKey> optGroupKey{};

    std::optional<query::Field> optQueryField{};
  };

  std::span<TrackFieldDefinition const> trackFieldDefinitions();
  TrackFieldDefinition const* trackFieldDefinition(TrackField field);
  std::optional<TrackField> trackFieldFromId(std::string_view id);
  std::optional<TrackField> trackFieldFromQueryField(query::Field field);
  std::string_view trackFieldId(TrackField field);

  std::optional<query::Field> trackFieldQueryField(TrackField field);
  std::string trackFieldFilterExpressionVariable(TrackField field);
  bool supportsTrackFieldFilterExpression(TrackField field);
  bool supportsTrackFieldValueCompletion(TrackField field);
} // namespace ao::rt
