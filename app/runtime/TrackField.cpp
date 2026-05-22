// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackField.h"

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
        .optSortField = TrackSortField::Artist,
        .optGroupKey = TrackGroupKey::Artist,
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
        .optSortField = TrackSortField::Album,
        .optGroupKey = TrackGroupKey::Album,
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
        .optSortField = TrackSortField::AlbumArtist,
        .optGroupKey = TrackGroupKey::AlbumArtist,
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
        .optSortField = TrackSortField::Genre,
        .optGroupKey = TrackGroupKey::Genre,
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
        .optSortField = TrackSortField::Composer,
        .optGroupKey = TrackGroupKey::Composer,
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
        .optSortField = TrackSortField::Work,
        .optGroupKey = TrackGroupKey::Work,
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
      },
      {
        .field = F::TotalDiscs,
        .id = "total-discs",
        .label = "Total Discs",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
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
      },
      {
        .field = F::TotalTracks,
        .id = "total-tracks",
        .label = "Total Tracks",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
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
      },
      {
        .field = F::Channels,
        .id = "channels",
        .label = "Channels",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      {
        .field = F::BitDepth,
        .id = "bit-depth",
        .label = "Bit Depth",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
      },
      {
        .field = F::Bitrate,
        .id = "bitrate",
        .label = "Bitrate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
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
    for (auto const& def : kDefinitions)
    {
      if (def.field == field)
      {
        return &def;
      }
    }

    return nullptr;
  }

  std::optional<TrackField> trackFieldFromId(std::string_view id)
  {
    for (auto const& def : kDefinitions)
    {
      if (def.id == id)
      {
        return def.field;
      }
    }

    return std::nullopt;
  }

  std::string_view trackFieldId(TrackField field)
  {
    if (auto const* def = trackFieldDefinition(field))
    {
      return def->id;
    }

    return {};
  }
} // namespace ao::rt
