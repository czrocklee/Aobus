// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::rt
{
  namespace
  {
    using F = TrackField;
    using Cat = TrackFieldCategory;
    using Q = query::Field;
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
        .optQueryField = Q::Title,
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
        .optQueryField = Q::ArtistId,
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
        .optQueryField = Q::AlbumId,
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
        .optQueryField = Q::AlbumArtistId,
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
        .optQueryField = Q::GenreId,
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
        .optQueryField = Q::ComposerId,
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
        .optQueryField = Q::ConductorId,
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
        .optQueryField = Q::EnsembleId,
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
        .optQueryField = Q::WorkId,
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
        .optQueryField = Q::MovementId,
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
        .optQueryField = Q::SoloistId,
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
        .optQueryField = Q::Year,
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
        .optQueryField = Q::DiscNumber,
      },
      {
        .field = F::DiscTotal,
        .id = "disc-total",
        .label = "Total Discs",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::DiscTotal,
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
        .optQueryField = Q::TrackNumber,
      },
      {
        .field = F::TrackTotal,
        .id = "track-total",
        .label = "Total Tracks",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::TrackTotal,
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
        .optQueryField = Q::MovementNumber,
      },
      {
        .field = F::MovementTotal,
        .id = "movement-total",
        .label = "Total Movements",
        .category = Cat::Metadata,
        .valueKind = Vk::Number,
        .presentable = true,
        .editable = true,
        .optQueryField = Q::MovementTotal,
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
        .optQueryField = Q::Duration,
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
        .optQueryField = Q::Codec,
      },
      {
        .field = F::SampleRate,
        .id = "sample-rate",
        .label = "Sample Rate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::SampleRate,
      },
      {
        .field = F::Channels,
        .id = "channels",
        .label = "Channels",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::Channels,
      },
      {
        .field = F::BitDepth,
        .id = "bit-depth",
        .label = "Bit Depth",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::BitDepth,
      },
      {
        .field = F::Bitrate,
        .id = "bitrate",
        .label = "Bitrate",
        .category = Cat::Technical,
        .valueKind = Vk::TechnicalText,
        .presentable = true,
        .optQueryField = Q::Bitrate,
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

    query::QueryVariableDescriptor const* queryVariableDescriptor(TrackFieldDefinition const& definition)
    {
      if (!definition.optQueryField)
      {
        return nullptr;
      }

      return query::findQueryVariableDescriptor(*definition.optQueryField);
    }
  } // namespace

  std::span<TrackFieldDefinition const> trackFieldDefinitions()
  {
    return kDefinitions;
  }

  TrackFieldDefinition const* trackFieldDefinition(TrackField field)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const it = std::ranges::find(kDefinitions, field, &TrackFieldDefinition::field);
    return it != kDefinitions.end() ? &*it : nullptr;
  }

  std::optional<TrackField> trackFieldFromId(std::string_view id)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const it = std::ranges::find(kDefinitions, id, &TrackFieldDefinition::id);
    return it != kDefinitions.end() ? std::optional{it->field} : std::nullopt;
  }

  std::optional<TrackField> trackFieldFromQueryField(query::Field field)
  {
    // NOLINTNEXTLINE(readability-qualified-auto) -- std::array iterator representations differ across libraries.
    auto const iter = std::ranges::find_if(kDefinitions,
                                           [field](TrackFieldDefinition const& definition)
                                           { return definition.optQueryField && *definition.optQueryField == field; });
    return iter != kDefinitions.end() ? std::optional{iter->field} : std::nullopt;
  }

  std::string_view trackFieldId(TrackField field)
  {
    if (auto const* def = trackFieldDefinition(field); def != nullptr)
    {
      return def->id;
    }

    return {};
  }

  std::optional<query::Field> trackFieldQueryField(TrackField const field)
  {
    if (auto const* const def = trackFieldDefinition(field); def != nullptr)
    {
      return def->optQueryField;
    }

    return std::nullopt;
  }

  std::string trackFieldFilterExpressionVariable(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);

    if (definition == nullptr)
    {
      return {};
    }

    auto const* const descriptor = queryVariableDescriptor(*definition);

    if (descriptor == nullptr)
    {
      return {};
    }

    return query::variableDisplayName(descriptor->type, descriptor->canonicalName);
  }

  bool supportsTrackFieldFilterExpression(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);
    return definition != nullptr && queryVariableDescriptor(*definition) != nullptr;
  }

  bool supportsTrackFieldValueCompletion(TrackField const field)
  {
    auto const* const definition = trackFieldDefinition(field);

    if (definition == nullptr || !definition->valueCompletion)
    {
      return false;
    }

    auto const* const descriptor = queryVariableDescriptor(*definition);
    return descriptor != nullptr && query::isDictionaryField(descriptor->field);
  }
} // namespace ao::rt
