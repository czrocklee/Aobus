// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ao::rt
{
  namespace
  {
    using F = TrackField;
    using Cat = TrackFieldCategory;
    using Vk = TrackFieldValueKind;

    constexpr auto kDefinitions = std::to_array<TrackFieldDefinition>({
      // --- Metadata: text ---
      {
        .field = F::Title,
        .id = "title",
        .label = "Title",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::Title,
        .filterExpressionVariable = "$title",
      },
      {
        .field = F::Artist,
        .id = "artist",
        .label = "Artist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Artist,
        .optGroupKey = TrackGroupKey::Artist,
        .filterExpressionVariable = "$artist",
      },
      {
        .field = F::Album,
        .id = "album",
        .label = "Album",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Album,
        .optGroupKey = TrackGroupKey::Album,
        .filterExpressionVariable = "$album",
      },
      {
        .field = F::AlbumArtist,
        .id = "album-artist",
        .label = "Album Artist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::AlbumArtist,
        .optGroupKey = TrackGroupKey::AlbumArtist,
        .filterExpressionVariable = "$albumArtist",
      },
      {
        .field = F::Genre,
        .id = "genre",
        .label = "Genre",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Genre,
        .optGroupKey = TrackGroupKey::Genre,
        .filterExpressionVariable = "$genre",
      },
      {
        .field = F::Composer,
        .id = "composer",
        .label = "Composer",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Composer,
        .optGroupKey = TrackGroupKey::Composer,
        .filterExpressionVariable = "$composer",
      },
      {
        .field = F::Conductor,
        .id = "conductor",
        .label = "Conductor",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Conductor,
        .optGroupKey = TrackGroupKey::Conductor,
        .filterExpressionVariable = "$conductor",
      },
      {
        .field = F::Ensemble,
        .id = "ensemble",
        .label = "Ensemble",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Ensemble,
        .optGroupKey = TrackGroupKey::Ensemble,
        .filterExpressionVariable = "$ensemble",
      },
      {
        .field = F::Work,
        .id = "work",
        .label = "Work",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .valueCompletion = true,
        .optSortField = TrackSortField::Work,
        .optGroupKey = TrackGroupKey::Work,
        .filterExpressionVariable = "$work",
      },
      {
        .field = F::Movement,
        .id = "movement",
        .label = "Movement",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = false,
        .valueCompletion = true,
        .optSortField = TrackSortField::Movement,
        .filterExpressionVariable = "$movement",
      },
      {
        .field = F::Soloist,
        .id = "soloist",
        .label = "Soloist",
        .category = Cat::Metadata,
        .valueKind = Vk::Text,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = false,
        .valueCompletion = true,
        .optSortField = TrackSortField::Soloist,
        .filterExpressionVariable = "$soloist",
      },
      // --- Metadata: number ---
      {
        .field = F::Year,
        .id = "year",
        .label = "Year",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .groupable = true,
        .optSortField = TrackSortField::Year,
        .optGroupKey = TrackGroupKey::Year,
        .filterExpressionVariable = "$year",
      },
      {
        .field = F::DiscNumber,
        .id = "disc-number",
        .label = "Disc",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::DiscNumber,
        .filterExpressionVariable = "$discNumber",
      },
      {
        .field = F::DiscTotal,
        .id = "disc-total",
        .label = "Total Discs",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .filterExpressionVariable = "$discTotal",
      },
      {
        .field = F::TrackNumber,
        .id = "track-number",
        .label = "Track",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::TrackNumber,
        .filterExpressionVariable = "$trackNumber",
      },
      {
        .field = F::TrackTotal,
        .id = "track-total",
        .label = "Total Tracks",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .filterExpressionVariable = "$trackTotal",
      },
      {
        .field = F::MovementNumber,
        .id = "movement-number",
        .label = "Movement No.",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .sortable = true,
        .optSortField = TrackSortField::Movement,
        .filterExpressionVariable = "$movementNumber",
      },
      {
        .field = F::MovementTotal,
        .id = "movement-total",
        .label = "Total Movements",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .filterExpressionVariable = "$movementTotal",
      },
      // --- Duration ---
      {
        .field = F::Duration,
        .id = "duration",
        .label = "Duration",
        .category = Cat::Technical,
        .valueKind = Vk::Duration,
        .presentable = true,
        .sortable = true,
        .optSortField = TrackSortField::Duration,
        .filterExpressionVariable = "@duration",
      },
      // --- Tags ---
      {
        .field = F::Tags,
        .id = "tags",
        .label = "Tags",
        .category = Cat::Tag,
        .valueKind = Vk::TagList,
        .presentable = true,
      },
      // --- Technical ---
      {
        .field = F::FilePath,
        .id = "file-path",
        .label = "File Path",
        .category = Cat::Technical,
        .valueKind = Vk::FilePath,
        .presentable = true,
      },
      {
        .field = F::Codec,
        .id = "codec",
        .label = "Codec",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      {
        .field = F::SampleRate,
        .id = "sample-rate",
        .label = "Sample Rate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .filterExpressionVariable = "@sampleRate",
      },
      {
        .field = F::Channels,
        .id = "channels",
        .label = "Channels",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .filterExpressionVariable = "@channels",
      },
      {
        .field = F::BitDepth,
        .id = "bit-depth",
        .label = "Bit Depth",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .filterExpressionVariable = "@bitDepth",
      },
      {
        .field = F::Bitrate,
        .id = "bitrate",
        .label = "Bitrate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .filterExpressionVariable = "@bitrate",
      },
      {
        .field = F::FileSize,
        .id = "file-size",
        .label = "File Size",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      {
        .field = F::ModifiedTime,
        .id = "modified-time",
        .label = "Modified",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      // --- Synthetic ---
      {
        .field = F::DisplayTrackNumber,
        .id = "display-track-number",
        .label = "Track #",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
      {
        .field = F::TechnicalSummary,
        .id = "technical-summary",
        .label = "Technical",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
      {
        .field = F::Quality,
        .id = "quality",
        .label = "Quality",
        .category = Cat::Synthetic,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .synthetic = true,
      },
    });

    static_assert(kDefinitions.size() == kTrackFieldCount, "Track Field registry must match kTrackFieldCount");
  } // namespace

  std::span<TrackFieldDefinition const> trackFieldDefinitions()
  {
    return kDefinitions;
  }

  TrackFieldDefinition const* trackFieldDefinition(TrackField field)
  {
    auto const it = std::ranges::find(kDefinitions, field, &TrackFieldDefinition::field);
    return it != kDefinitions.end() ? &*it : nullptr;
  }

  std::optional<TrackField> trackFieldFromId(std::string_view id)
  {
    auto const it = std::ranges::find(kDefinitions, id, &TrackFieldDefinition::id);
    return it != kDefinitions.end() ? std::optional{it->field} : std::nullopt;
  }

  std::string_view trackFieldId(TrackField field)
  {
    if (auto const* def = trackFieldDefinition(field); def != nullptr)
    {
      return def->id;
    }

    return {};
  }

  std::string_view trackFieldFilterExpressionVariable(TrackField const field)
  {
    if (auto const* const def = trackFieldDefinition(field); def != nullptr)
    {
      return def->filterExpressionVariable;
    }

    return {};
  }

  bool supportsTrackFieldFilterExpression(TrackField const field)
  {
    return !trackFieldFilterExpressionVariable(field).empty();
  }

  bool supportsTrackFieldValueCompletion(TrackField const field)
  {
    if (auto const* const def = trackFieldDefinition(field); def != nullptr)
    {
      return def->valueCompletion;
    }

    return false;
  }
} // namespace ao::rt
